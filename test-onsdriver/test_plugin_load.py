'''
Test plugin is loaded
'''

import base64
import glob
import os
import shutil
import subprocess
import struct
import tempfile
import time
import uuid
import unittest
import zlib
from types import SimpleNamespace
from unittest.mock import Mock
from onsdriver import obstest, obsui

PLUGIN_NAME = 'face-tracker'


def decode_obs_screenshot(image_data):
    """Decode OBS's non-interlaced 8-bit RGB/RGBA PNG screenshots."""
    encoded = image_data.partition(',')[2] or image_data
    png = base64.b64decode(encoded)
    if not png.startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError('OBS screenshot is not a PNG image')

    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    offset = 8
    while offset + 12 <= len(png):
        length = struct.unpack_from('>I', png, offset)[0]
        chunk_type = png[offset + 4 : offset + 8]
        chunk_data = png[offset + 8 : offset + 8 + length]
        if len(chunk_data) != length:
            raise ValueError('truncated PNG chunk')
        if chunk_type == b'IHDR':
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                '>IIBBBBB', chunk_data
            )
        elif chunk_type == b'IDAT':
            compressed.extend(chunk_data)
        elif chunk_type == b'IEND':
            break
        offset += length + 12

    if bit_depth != 8 or color_type not in (2, 6) or interlace != 0:
        raise ValueError('unsupported OBS screenshot PNG format')
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(compressed)
    if len(raw) != height * (stride + 1):
        raise ValueError('invalid PNG scanline data')

    pixels = bytearray(height * stride)
    source_offset = 0
    for row in range(height):
        filter_type = raw[source_offset]
        source_offset += 1
        row_offset = row * stride
        for column in range(stride):
            value = raw[source_offset + column]
            left = pixels[row_offset + column - channels] if column >= channels else 0
            above = pixels[row_offset - stride + column] if row else 0
            upper_left = (
                pixels[row_offset - stride + column - channels]
                if row and column >= channels
                else 0
            )
            if filter_type == 1:
                value += left
            elif filter_type == 2:
                value += above
            elif filter_type == 3:
                value += (left + above) // 2
            elif filter_type == 4:
                predictor = left + above - upper_left
                distances = (
                    abs(predictor - left),
                    abs(predictor - above),
                    abs(predictor - upper_left),
                )
                value += (left, above, upper_left)[distances.index(min(distances))]
            elif filter_type != 0:
                raise ValueError(f'unsupported PNG filter {filter_type}')
            pixels[row_offset + column] = value & 0xFF
        source_offset += stride

    if channels == 4:
        return width, height, bytes(pixels)
    rgba = bytearray(width * height * 4)
    for index in range(width * height):
        rgba[index * 4 : index * 4 + 3] = pixels[index * 3 : index * 3 + 3]
        rgba[index * 4 + 3] = 255
    return width, height, bytes(rgba)


def screenshots_match_with_render_tolerance(expected, actual):
    """Allow tiny GPU color-rounding differences, while rejecting a different crop."""
    try:
        expected_width, expected_height, expected_pixels = decode_obs_screenshot(expected)
        actual_width, actual_height, actual_pixels = decode_obs_screenshot(actual)
    except (ValueError, zlib.error, struct.error):
        return False
    if (expected_width, expected_height) != (actual_width, actual_height):
        return False
    differences = [abs(a - b) for a, b in zip(expected_pixels, actual_pixels)]
    mean_difference = sum(differences) / len(differences)
    maximum_difference = max(differences, default=0)
    return mean_difference <= 1.0 and maximum_difference <= 8


def screenshot_has_visible_pixels(image_data):
    """Return whether an OBS screenshot contains rendered image content."""
    try:
        _, _, pixels = decode_obs_screenshot(image_data)
    except (ValueError, zlib.error, struct.error):
        return False
    return any(
        pixels[index] > 8 or pixels[index + 1] > 8 or pixels[index + 2] > 8
        for index in range(0, len(pixels), 4)
    )


def firsttime_setup_was_force_stopped(logsdir):
    firsttime_logs = glob.glob(os.path.join(logsdir, 'firsttime-*.txt'))
    for logfile in firsttime_logs:
        try:
            with open(logfile, 'r', encoding='utf-8') as fr:
                was_force_stopped = '[shutdown-plugin] force exit.' in fr.read()
        except OSError:
            continue
        if was_force_stopped:
            return True
    return False


def clear_inherited_setup_shutdown_sentinels(config_path, logsdir):
    """Clear OBS shutdown markers only when onsdriver records its forced exit."""
    safe_mode_sentinel = os.path.join(config_path, 'safe_mode')
    crash_sentinel_dir = os.path.join(config_path, '.sentinel')
    crash_sentinels = []
    if os.path.isdir(crash_sentinel_dir):
        crash_sentinels = [
            os.path.join(crash_sentinel_dir, name)
            for name in os.listdir(crash_sentinel_dir)
            if name.startswith('run_')
            and os.path.isfile(os.path.join(crash_sentinel_dir, name))
        ]

    has_inherited_sentinel = os.path.exists(safe_mode_sentinel) or bool(crash_sentinels)
    setup_was_force_stopped = firsttime_setup_was_force_stopped(logsdir)
    if not has_inherited_sentinel:
        return setup_was_force_stopped
    if setup_was_force_stopped:
        for sentinel in [safe_mode_sentinel, *crash_sentinels]:
            if not os.path.exists(sentinel):
                continue
            try:
                os.remove(sentinel)
            except OSError as error:
                raise RuntimeError(
                    f'Unable to remove inherited OBS shutdown marker: {sentinel}'
                ) from error
        return True
    raise RuntimeError(
        'OBS setup profile has an unclean-shutdown sentinel '
        'without an onsdriver force-exit record'
    )


def write_synthetic_person_png(output_path):
    """Create a two-person static OBS image from the cartoon fixture."""
    fixture_path = os.path.join(
        os.path.dirname(__file__),
        os.pardir,
        'tests',
        'fixtures',
        'offline-synthetic-person-320x360.ppm',
    )
    with open(fixture_path, 'rb') as fixture_file:
        magic, dimensions, maximum, rgb_pixels = fixture_file.read().split(b'\n', 3)
    if magic.strip() != b'P6' or maximum.strip() != b'255':
        raise ValueError('synthetic face fixture must be an 8-bit P6 PPM')
    source_width, source_height = map(int, dimensions.split())
    if len(rgb_pixels) != source_width * source_height * 3:
        raise ValueError('synthetic face fixture has an invalid pixel payload')

    # Reduce the face to 80% of its original pixel size as well as using the
    # smaller canvas. The full-resolution face was too large for the framing
    # assertion, while the 2/3-size normalization was too small for reliable
    # HOG tracking on slower Windows OBS runners.
    # Keep the OBS source at the detector's working resolution. The previous
    # 1280x720 fixture was immediately halved to 640x360 by the adapter, adding
    # render load to this operator-workflow test without adding detector detail.
    width = source_width * 2
    height = width * 9 // 16
    person_width = max(1, (source_width * 2 + 2) // 5)
    person_height = max(1, (source_height * 2 + 2) // 5)
    y_offset = (height - person_height) // 2
    x_offsets = (width // 4 - person_width // 2, 3 * width // 4 - person_width // 2)
    if y_offset < 0 or any(x_offset < 0 or x_offset + person_width > width for x_offset in x_offsets):
        raise ValueError('synthetic face fixture must fit inside the 16:9 canvas')
    padded_pixels = bytearray(width * height * 3)
    for x_offset in x_offsets:
        for y in range(person_height):
            source_y = min(source_height - 1, ((2 * y + 1) * source_height) // (2 * person_height))
            for x in range(person_width):
                source_x = min(source_width - 1, ((2 * x + 1) * source_width) // (2 * person_width))
                source_offset = (source_y * source_width + source_x) * 3
                destination_offset = ((y + y_offset) * width + x + x_offset) * 3
                padded_pixels[destination_offset : destination_offset + 3] = rgb_pixels[
                    source_offset : source_offset + 3
                ]
    image_data = b''.join(
        b'\x00' + padded_pixels[y * width * 3 : (y + 1) * width * 3]
        for y in range(height)
    )

    def chunk(kind, data):
        body = kind + data
        checksum = zlib.crc32(body) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + body + struct.pack('>I', checksum)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(image_data))
    png += chunk(b'IEND', b'')
    with open(output_path, 'wb') as output_file:
        output_file.write(png)


def write_synthetic_signal_png(output_path, width=320, height=180):
    """Write a small solid-color PNG for deterministic OBS signal tests."""

    def chunk(kind, data):
        body = kind + data
        checksum = zlib.crc32(body) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + body + struct.pack('>I', checksum)

    row = bytes((0x4A, 0x76, 0xC4)) * width
    image_data = b''.join(b'\x00' + row for _ in range(height))
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(image_data))
    png += chunk(b'IEND', b'')
    with open(output_path, 'wb') as output_file:
        output_file.write(png)


class SetupShutdownSentinelTest(unittest.TestCase):
    def test_absent_sentinel_is_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            os.makedirs(config_path)
            os.makedirs(logsdir)

            self.assertFalse(clear_inherited_setup_shutdown_sentinels(config_path, logsdir))

    def test_forced_setup_shutdown_clears_only_the_inherited_sentinel(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            os.makedirs(config_path)
            os.makedirs(logsdir)
            sentinel = os.path.join(config_path, 'safe_mode')
            with open(sentinel, 'w', encoding='utf-8'):
                pass
            with open(os.path.join(logsdir, 'firsttime-setup.txt'), 'w', encoding='utf-8') as fw:
                fw.write('[shutdown-plugin] force exit.\n')

            self.assertTrue(clear_inherited_setup_shutdown_sentinels(config_path, logsdir))
            self.assertFalse(os.path.exists(sentinel))

    def test_unexplained_sentinel_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            os.makedirs(config_path)
            os.makedirs(logsdir)
            sentinel = os.path.join(config_path, 'safe_mode')
            with open(sentinel, 'w', encoding='utf-8'):
                pass

            with self.assertRaisesRegex(RuntimeError, 'without an onsdriver force-exit record'):
                clear_inherited_setup_shutdown_sentinels(config_path, logsdir)
            self.assertTrue(os.path.exists(sentinel))

    def test_force_stopped_setup_clears_obs_32_crash_sentinels(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            sentinel_dir = os.path.join(config_path, '.sentinel')
            os.makedirs(sentinel_dir)
            os.makedirs(logsdir)
            sentinels = [os.path.join(sentinel_dir, f'run_{index}') for index in range(2)]
            for sentinel in sentinels:
                with open(sentinel, 'w', encoding='utf-8'):
                    pass
            unrelated_marker = os.path.join(sentinel_dir, 'keep.txt')
            with open(unrelated_marker, 'w', encoding='utf-8'):
                pass
            with open(os.path.join(logsdir, 'firsttime-setup.txt'), 'w', encoding='utf-8') as fw:
                fw.write('[shutdown-plugin] force exit.\n')

            self.assertTrue(clear_inherited_setup_shutdown_sentinels(config_path, logsdir))
            self.assertFalse(any(os.path.exists(sentinel) for sentinel in sentinels))
            self.assertTrue(os.path.exists(unrelated_marker))

    def test_unexplained_obs_32_crash_sentinel_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            sentinel_dir = os.path.join(config_path, '.sentinel')
            os.makedirs(sentinel_dir)
            os.makedirs(logsdir)
            sentinel = os.path.join(sentinel_dir, 'run_inherited')
            with open(sentinel, 'w', encoding='utf-8'):
                pass

            with self.assertRaisesRegex(RuntimeError, 'without an onsdriver force-exit record'):
                clear_inherited_setup_shutdown_sentinels(config_path, logsdir)
            self.assertTrue(os.path.exists(sentinel))


class PluginLoadTest(obstest.OBSTest):
    'Test the plugin is loaded by OBS Studio'

    @staticmethod
    def _input_names(client):
        response = client.send('GetInputList', {})
        return {item['inputName'] for item in response.inputs}

    @staticmethod
    def _input_uuid(client, input_name):
        response = client.send('GetInputList', {})
        for item in response.inputs:
            if item['inputName'] == input_name:
                source_uuid = item.get('inputUuid')
                if source_uuid:
                    return source_uuid
                break
        raise AssertionError(f'OBS did not report a UUID for input {input_name!r}')

    def _wait_for_input_presence(self, client, input_name, expected, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (input_name in self._input_names(client)) == expected:
                return
            time.sleep(0.1)
        expectation = 'appear' if expected else 'be removed'
        self.fail(f'Timed out waiting for OBS input {input_name!r} to {expectation}')

    def _wait_for_log_text(self, text, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            logfile = self.obs.get_logfile()
            try:
                with open(logfile, 'r', encoding='utf-8', errors='replace') as fr:
                    if text in fr.read():
                        return
            except OSError:
                pass
            time.sleep(0.1)
        self.fail(f'Timed out waiting for OBS log message {text!r}')

    def _screenshot(self, client, source_name):
        response = client.send('GetSourceScreenshot', {
            'sourceName': source_name,
            'imageFormat': 'png',
            'imageWidth': 16,
            'imageHeight': 16,
        })
        image_data = response.image_data
        if not image_data:
            raise AssertionError(f'OBS returned an empty screenshot for source {source_name!r}')
        return image_data

    def _wait_for_screenshot(self, client, source_name, predicate, timeout=5.0):
        deadline = time.monotonic() + timeout
        last_image = None
        last_error = None
        while time.monotonic() < deadline:
            try:
                last_image = self._screenshot(client, source_name)
                last_error = None
                if predicate(last_image):
                    return last_image
            except Exception as error:  # OBS may need a frame after a scene or source change.
                last_error = error
            time.sleep(0.1)
        if last_error:
            self.fail(f'Unable to capture {source_name!r}: {last_error}')
        self.fail(f'Timed out waiting for the expected frame from {source_name!r}')

    def _wait_for_media_input_state(self, client, input_name, expected_state, timeout=10.0):
        deadline = time.monotonic() + timeout
        last_state = None
        while time.monotonic() < deadline:
            response = client.send('GetMediaInputStatus', {'inputName': input_name})
            last_state = getattr(response, 'media_state', None)
            if last_state is None and isinstance(response, dict):
                last_state = response.get('mediaState') or response.get('media_state')
            if last_state == expected_state:
                return response
            time.sleep(0.1)
        self.fail(
            f'OBS media input {input_name!r} did not reach {expected_state!r}: {last_state!r}'
        )

    @staticmethod
    def _find_ui_widget(widgets, accessible_name):
        pending = list(widgets)
        while pending:
            widget = pending.pop()
            if widget.get('accessibleName') == accessible_name:
                return widget
            pending.extend(widget.get('children', []))
        return None

    @classmethod
    def _find_menu_path(cls, actions, text, parent=()):
        for action in actions:
            action_text = action.get('text', '')
            path = parent + ({'text': action_text},)
            if text.lower() in action_text.replace('&', '').lower():
                return list(path)
            child_path = cls._find_menu_path(action.get('menu', []), text, path)
            if child_path:
                return child_path
        return None

    @classmethod
    def _ui_widget_path(cls, widgets, property_name, property_value, parents=()):
        for widget in widgets:
            locator = {'className': widget.get('className', '')}
            for key in ('objectName', 'accessibleName', 'text'):
                value = widget.get(key)
                if value:
                    locator[key] = value
            path = parents + (locator,)
            if widget.get(property_name) == property_value:
                return list(path)
            child_path = cls._ui_widget_path(
                widget.get('children', []), property_name, property_value, path
            )
            if child_path:
                return child_path
        return None

    @classmethod
    def _invoke_ui_widget_from_tree(cls, ui, widget_tree, property_name, property_value, method, **arguments):
        path = cls._ui_widget_path(widget_tree.get('children', []), property_name, property_value)
        if path is None:
            raise AssertionError(f'UI widget {property_name}={property_value!r} was not found')
        return ui.request(
            'widget-invoke',
            {
                'path': path,
                'method': method,
                **arguments,
            },
        )

    @classmethod
    def _invoke_ui_widget(cls, ui, property_name, property_value, method, **arguments):
        widget_tree = ui.request('widget-list', {})
        return cls._invoke_ui_widget_from_tree(
            ui, widget_tree, property_name, property_value, method, **arguments
        )

    def _wait_for_ui_widget(self, ui, accessible_name, *, enabled=None, timeout=10.0):
        deadline = time.monotonic() + timeout
        last_widget = None
        while time.monotonic() < deadline:
            response = ui.request('widget-list', {})
            last_widget = self._find_ui_widget(response.get('children', []), accessible_name)
            if last_widget is not None and (enabled is None or last_widget.get('enabled') is enabled):
                return last_widget
            time.sleep(0.1)
        self.fail(f'UI widget {accessible_name!r} did not reach enabled={enabled}: {last_widget!r}')

    def _wait_for_ui_item_count(self, ui, accessible_name, minimum_count, timeout=10.0):
        deadline = time.monotonic() + timeout
        last_widget = None
        while time.monotonic() < deadline:
            response = ui.request('widget-list', {})
            last_widget = self._find_ui_widget(response.get('children', []), accessible_name)
            if last_widget is not None and last_widget.get('count', 0) >= minimum_count:
                return last_widget
            time.sleep(0.1)
        self.fail(
            f'UI widget {accessible_name!r} did not reach count>={minimum_count}: {last_widget!r}'
        )

    def _wait_for_ui_status(self, ui, text, timeout=10.0):
        deadline = time.monotonic() + timeout
        last_status = None
        while time.monotonic() < deadline:
            response = ui.request('widget-list', {})
            widget = self._find_ui_widget(response.get('children', []), 'FaithfulFrame tracking status')
            last_status = widget.get('text') if widget else None
            if last_status and text in last_status:
                return last_status
            time.sleep(0.1)
        self.fail(f'FaithfulFrame status did not contain {text!r}: {last_status!r}')

    def _wait_for_ui_status_any(self, ui, texts, timeout=10.0):
        deadline = time.monotonic() + timeout
        last_status = None
        while time.monotonic() < deadline:
            response = ui.request('widget-list', {})
            widget = self._find_ui_widget(response.get('children', []), 'FaithfulFrame tracking status')
            last_status = widget.get('text') if widget else None
            if last_status and any(text in last_status for text in texts):
                return last_status
            time.sleep(0.1)
        self.fail(f'FaithfulFrame status did not contain any of {texts!r}: {last_status!r}')

    def _wait_for_ui_zoom_above(self, ui, minimum_zoom, timeout=10.0):
        deadline = time.monotonic() + timeout
        last_status = None
        while time.monotonic() < deadline:
            response = ui.request('widget-list', {})
            widget = self._find_ui_widget(response.get('children', []), 'FaithfulFrame tracking status')
            last_status = widget.get('text') if widget else None
            if last_status and 'Following the selected speaker' in last_status:
                zoom_marker = 'Current zoom: '
                if zoom_marker in last_status:
                    try:
                        zoom_text = last_status.split(zoom_marker, 1)[1].split('×', 1)[0]
                        if float(zoom_text) > minimum_zoom:
                            return last_status
                    except ValueError:
                        pass
            time.sleep(0.1)
        self.fail(f'FaithfulFrame zoom did not exceed {minimum_zoom}× after selection: {last_status!r}')

    def setUp(self):
        # Keep the OBSExec instance even if startup checking times out, so the
        # failed launch's log and stderr can be saved and its process cleaned up.
        super().setUp(run=False)
        self._startup_succeeded = False
        self.addCleanup(self._preserve_final_log)
        # onsdriver's first-time setup can force-stop OBS after its shutdown
        # timeout, leaving a version-specific sentinel that blocks the next
        # launch. Clear inherited markers only when its log confirms the force
        # exit.
        try:
            setup_was_force_stopped = clear_inherited_setup_shutdown_sentinels(
                self.obs.config.path, os.environ.get('ONSDRIVER_LOGS', 'logs')
            )
        except RuntimeError as error:
            self.fail(str(error))
        if setup_was_force_stopped:
            print('Cleared inherited OBS shutdown marker after onsdriver force-stopped setup')
        diagnostics_name = 'FAITHFULFRAME_TEST_DETECTOR_DIAGNOSTICS'
        previous_diagnostics = os.environ.get(diagnostics_name)
        if self.id().split('.')[-1].startswith('test_safe_adapter_'):
            # Safe-adapter OBS tests keep native lifecycle diagnostics so CI
            # failures can distinguish a source-binding problem from detector
            # candidate churn.
            os.environ[diagnostics_name] = '1'
        try:
            self.obs.run()
        finally:
            if previous_diagnostics is None:
                os.environ.pop(diagnostics_name, None)
            else:
                os.environ[diagnostics_name] = previous_diagnostics
        self._startup_succeeded = True

    def _preserve_final_log(self):
        # OBSTest checks the leak summary in tearDown before moving the log.
        # Preserve the final OBS log even when that assertion fails so CI
        # artifacts contain the shutdown diagnostics.
        process = getattr(self.obs, 'proc_obs', None)
        if process and process.poll() is None:
            try:
                process.terminate()
            except OSError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    process.kill()
                except OSError:
                    pass
                process.wait()

        logsdir = os.environ.get('ONSDRIVER_LOGS', 'logs')
        os.makedirs(logsdir, exist_ok=True)
        logfile = None
        try:
            logfile = self.obs.get_logfile()
        except OSError:
            pass

        if logfile and os.path.isfile(logfile):
            filename = f'diagnostic-{self.name.replace(os.sep, "_")}.log'
            destination = os.path.join(logsdir, filename)
            if os.path.abspath(logfile) != os.path.abspath(destination):
                shutil.copy2(logfile, destination)
        elif not self._startup_succeeded:
            filename = f'diagnostic-{self.name.replace(os.sep, "_")}-startup.txt'
            with open(os.path.join(logsdir, filename), 'w', encoding='utf-8') as fw:
                fw.write(f'OBS log path: {logfile!r}\n')

        stderr = getattr(self.obs, '_tmp_stderr', None)
        if stderr:
            try:
                stderr.seek(0)
                contents = stderr.read()
            except (OSError, ValueError):
                contents = b''
            if contents:
                filename = f'diagnostic-{self.name.replace(os.sep, "_")}-stderr.txt'
                with open(os.path.join(logsdir, filename), 'wb') as fw:
                    fw.write(contents)

    def tearDown(self):
        # onsdriver's shutdown() enables an unsafe force-exit after five
        # seconds. That can terminate OBS before its normal leak summary is
        # written, making plugin teardown impossible to distinguish from a
        # slow runner. Ask the same shutdown plugin to stop outputs and exit,
        # but let the CI step timeout handle a genuine hang.
        client = self.obs.get_obsws()
        response = client.send('CallVendorRequest', {
            'vendorName': 'shutdown-plugin',
            'requestType': 'shutdown',
            'requestData': {
                'reason': f'requested through onsdriver by {self.id()}',
                'support_url': 'https://github.com/noris-plugins-for-obs/onsdriver/issues',
                'force': True,
                'exit_timeout': 0.0,
            }
        })
        if response.response_data != {}:
            raise ValueError(f'shutdown request returned {response.response_data}')
        del client
        self.obs.wait()
        self.assertEqual(self.memory_leak(), 0)
        self.move_log(prefix=self.name + '-')

    def test_log(self):
        'Check log file'
        search_text = f' [{PLUGIN_NAME}] '
        loaded = False
        with open(self.obs.get_logfile(), 'r', encoding='utf-8') as fr:
            for line in fr:
                if search_text in line:
                    print(line)
                    loaded = True
                    break
        self.assertTrue(loaded)

    def test_safe_adapter_dock_controls_update_tracking_state(self):
        'Exercise the live volunteer controls through OBS Qt widgets'
        client = self.obs.get_obsws()
        ui = obsui.OBSUI(client)
        suffix = uuid.uuid4().hex
        tracked_scene = f'FaithfulFrame dock controls {suffix}'
        camera_name = f'FaithfulFrame dock camera {suffix}'
        adapter_name = f'FaithfulFrame dock adapter {suffix}'
        original_scene = None
        created_scene = False
        created_inputs = []
        dock_created = False

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            color_kinds = [
                kind
                for kind in kinds_response.input_kinds
                if kind == 'color_source' or kind.startswith('color_source_v')
            ]
            self.assertTrue(color_kinds, 'OBS color source is required for hardware-free control testing')
            color_kind = max(
                color_kinds,
                key=lambda kind: int(kind.rsplit('_v', 1)[1]) if '_v' in kind else 0,
            )

            client.send('CreateScene', {'sceneName': tracked_scene})
            created_scene = True
            client.send('SetCurrentProgramScene', {'sceneName': tracked_scene})
            client.send('CreateInput', {
                'sceneName': tracked_scene,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF336699, 'width': 320, 'height': 180},
            })
            created_inputs.append(camera_name)
            camera_uuid = self._input_uuid(client, camera_name)
            client.send('CreateInput', {
                'sceneName': tracked_scene,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': camera_name,
                    'target_uuid': camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            created_inputs.append(adapter_name)
            self._wait_for_screenshot(client, adapter_name, bool)

            menu_tree = ui.menu_list().get('menu', [])
            dock_menu_path = self._find_menu_path(menu_tree, 'New FaithfulFrame Dock')
            self.assertIsNotNone(dock_menu_path, 'FaithfulFrame dock action must be available in OBS Tools')
            ui.request('menu-trigger', {'path': dock_menu_path})
            dock_created = True

            source_selector_name = 'Camera source with FaithfulFrame tracking filter'
            source_selector = self._wait_for_ui_widget(ui, source_selector_name)
            self.assertIn('arrow keys', source_selector.get('accessibleDescription', '').lower())
            self.assertIn('scrolling over', source_selector.get('accessibleDescription', '').lower())
            self._invoke_ui_widget(ui, 'accessibleName', source_selector_name, 'setCurrentIndex', arg1=1)
            self._wait_for_ui_status(ui, f'Tracking source: {camera_name}')
            status_icon = self._wait_for_ui_widget(ui, 'Tracking state: Manual')
            self.assertIn('adjacent status text', status_icon.get('accessibleDescription', '').lower())
            self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=True)

            # The face list and selection control remain keyboard-addressable
            # when no face is present; selection stays disabled until evidence
            # appears in the live preview.
            preview = self._wait_for_ui_widget(ui, 'Live camera preview')
            self.assertIn('keyboard users', preview.get('accessibleDescription', '').lower())
            candidates = self._wait_for_ui_widget(ui, 'Detected face candidates')
            self.assertIn('arrow keys', candidates.get('accessibleDescription', '').lower())
            self.assertIn('scrolling over', candidates.get('accessibleDescription', '').lower())
            select_control = self._wait_for_ui_widget(
                ui, 'Select the highlighted face as the speaker', enabled=False
            )
            self.assertIn('press space', select_control.get('accessibleDescription', '').lower())

            self._invoke_ui_widget(ui, 'accessibleName', 'Enable automatic speaker following', 'click')
            status = self._wait_for_ui_status_any(
                ui,
                (
                    'No speaker selected; choose a visible face',
                    'Selection or command expired; choose again from the current preview',
                    'Command rejected;',
                ),
            )
            if 'No speaker selected; choose a visible face' not in status:
                self.assertTrue('rejected' in status.lower() or 'expired' in status.lower(), status)
                self._wait_for_ui_widget(ui, 'Tracking state: Manual')
                self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=True)
                self._invoke_ui_widget(ui, 'accessibleName', 'Enable automatic speaker following', 'click')
                self._wait_for_ui_status(ui, 'No speaker selected; choose a visible face')
            self._wait_for_ui_widget(ui, 'Tracking state: Searching')

            self._invoke_ui_widget(ui, 'accessibleName', 'Freeze automatic tracking', 'click')
            self._wait_for_ui_status(ui, 'Frozen; press Resume when ready')
            self._wait_for_ui_widget(ui, 'Tracking state: Frozen')
            self._wait_for_ui_widget(ui, 'Resume automatic tracking', enabled=True)

            self._invoke_ui_widget(ui, 'accessibleName', 'Resume automatic tracking', 'click')
            self._wait_for_ui_status(ui, 'Manual; select a face before following')
            self._wait_for_ui_widget(ui, 'Tracking state: Manual')
            self._invoke_ui_widget(ui, 'accessibleName', 'Enable automatic speaker following', 'click')
            self._wait_for_ui_status(ui, 'No speaker selected; choose a visible face')

            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Stop automatic following and hold the current framing',
                'click',
            )
            self._wait_for_ui_status(ui, 'Manual override active')

            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Reset framing to the full camera view',
                'click',
            )
            self._wait_for_ui_status(ui, 'Wide framing; select a face to follow again')
        finally:
            if dock_created:
                try:
                    self._invoke_ui_widget(ui, 'objectName', 'FTDock_ft_dock', 'removeDock')
                except Exception:
                    pass

            try:
                if original_scene:
                    client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            except Exception:
                pass

            try:
                names = self._input_names(client)
                for input_name in created_inputs:
                    if input_name in names:
                        client.send('RemoveInput', {'inputName': input_name})
            except Exception:
                pass

            if created_scene:
                try:
                    client.send('RemoveScene', {'sceneName': tracked_scene})
                except Exception:
                    pass

    def test_safe_adapter_keyboard_selects_synthetic_speaker(self):
        'Select a detector-confirmed synthetic face and verify only the adapter crops'
        client = self.obs.get_obsws()
        ui = obsui.OBSUI(client)
        suffix = uuid.uuid4().hex
        tracked_scene = f'FaithfulFrame synthetic speaker {suffix}'
        fallback_scene = f'FaithfulFrame synthetic fallback {suffix}'
        camera_name = f'FaithfulFrame synthetic face input {suffix}'
        fallback_name = f'FaithfulFrame synthetic fallback camera {suffix}'
        adapter_name = f'FaithfulFrame synthetic face adapter {suffix}'
        original_scene = None
        scene_created = False
        fallback_scene_created = False
        created_inputs = []
        dock_created = False
        temporary = tempfile.TemporaryDirectory(prefix='faithfulframe-synthetic-face-')
        # Keep the source image alive until OBS exits on platforms that retain
        # the decoded image for the lifetime of the OBS source.
        self.addCleanup(temporary.cleanup)

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            self.assertIn(
                'image_source',
                kinds_response.input_kinds,
                'OBS Image Source is required for hardware-free face-selection testing',
            )
            image_path = os.path.join(temporary.name, 'synthetic-person.png')
            write_synthetic_person_png(image_path)

            client.send('CreateScene', {'sceneName': tracked_scene})
            scene_created = True
            client.send('SetCurrentProgramScene', {'sceneName': tracked_scene})
            client.send('CreateInput', {
                'sceneName': tracked_scene,
                'inputName': camera_name,
                'inputKind': 'image_source',
                'inputSettings': {
                    'file': image_path,
                    'unload': False,
                },
            })
            created_inputs.append(camera_name)
            camera_uuid = self._input_uuid(client, camera_name)
            client.send('CreateInput', {
                'sceneName': tracked_scene,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': camera_name,
                    'target_uuid': camera_uuid,
                    'safe_tracking_enabled': True,
                    # Leave enough recovery time for the dock controls to be
                    # polled while this test verifies explicit rearming.
                    'safe_tracking_loss_grace_seconds': 15.0,
                    # Use a non-default framing target for this synthetic speaker workflow.
                    'safe_tracking_target_coverage_percent': 60.0,
                    # The synthetic source is already 640x360; keep the same
                    # detector working resolution without making OBS render a
                    # larger image for this operator-workflow test.
                    'scale': 1.0,
                },
            })
            created_inputs.append(adapter_name)

            adapter_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertEqual(
                adapter_settings.input_settings.get('safe_tracking_target_coverage_percent'),
                60.0,
            )

            camera_before = self._wait_for_screenshot(client, camera_name, bool)
            adapter_before = self._wait_for_screenshot(client, adapter_name, bool)
            menu_tree = ui.menu_list().get('menu', [])
            dock_menu_path = self._find_menu_path(menu_tree, 'New FaithfulFrame Dock')
            self.assertIsNotNone(dock_menu_path, 'FaithfulFrame dock action must be available in OBS Tools')
            ui.request('menu-trigger', {'path': dock_menu_path})
            dock_created = True

            source_selector_name = 'Camera source with FaithfulFrame tracking filter'
            self._wait_for_ui_widget(ui, source_selector_name)
            self._invoke_ui_widget(ui, 'accessibleName', source_selector_name, 'setCurrentIndex', arg1=1)
            self._wait_for_ui_status(ui, f'Tracking source: {camera_name}')
            self._wait_for_ui_widget(ui, 'Tracking state: Manual')

            select_control = 'Select the highlighted face as the speaker'
            try:
                # The synthetic detector fixture intentionally produces short
                # candidate windows: dlib periodically reseeds its tracker,
                # and the UI rejects handles after their 500 ms freshness
                # window. Retry the volunteer's keyboard selection while a
                # current candidate is available instead of turning one stale
                # poll into a flaky failure (or weakening production expiry).
                selection_deadline = time.monotonic() + 30.0
                selection_applied = False
                candidate_visible_polls = 0
                selection_attempts = 0
                last_selection_error = None
                last_candidates_widget = None
                while time.monotonic() < selection_deadline:
                    widget_tree = ui.request('widget-list', {})
                    widgets = widget_tree.get('children', [])
                    status_widget = self._find_ui_widget(widgets, 'FaithfulFrame tracking status')
                    status_text = status_widget.get('text', '') if status_widget else ''
                    if 'Following the selected speaker' in status_text or self._find_ui_widget(
                        widgets, 'Tracking state: Locked'
                    ):
                        selection_applied = True
                        break

                    candidates_widget = self._find_ui_widget(widgets, 'Detected face candidates')
                    last_candidates_widget = candidates_widget
                    # The first combo item is the placeholder; require two
                    # detected faces before selecting one of them.
                    if candidates_widget and candidates_widget.get('count', 0) >= 3:
                        candidate_visible_polls += 1
                        # Candidate observations expire quickly. Reuse this
                        # widget tree for both actions instead of making more
                        # remote inspection calls between seeing the handle
                        # and selecting it. The dock and session still validate
                        # its identity, generations, and expiry at application.
                        self._invoke_ui_widget_from_tree(
                            ui,
                            widget_tree,
                            'accessibleName',
                            'Detected face candidates',
                            'setCurrentIndex',
                            arg1=1,
                        )
                        try:
                            selection_attempts += 1
                            self._invoke_ui_widget_from_tree(
                                ui, widget_tree, 'accessibleName', select_control, 'click'
                            )
                        except AssertionError as error:
                            last_selection_error = error
                            time.sleep(0.05)
                            continue

                        # Candidate observations are deliberately short-lived.
                        # The command may safely no-op if they expire while
                        # crossing the UI bridge, so wait briefly for LOCKED
                        # and retry only from a newly published candidate.
                        attempt_deadline = min(time.monotonic() + 0.75, selection_deadline)
                        while time.monotonic() < attempt_deadline:
                            attempt_widgets = ui.request('widget-list', {}).get('children', [])
                            attempt_status = self._find_ui_widget(
                                attempt_widgets, 'FaithfulFrame tracking status'
                            )
                            attempt_status_text = attempt_status.get('text', '') if attempt_status else ''
                            if (
                                'Following the selected speaker' in attempt_status_text
                                or self._find_ui_widget(attempt_widgets, 'Tracking state: Locked')
                            ):
                                selection_applied = True
                                break
                            time.sleep(0.05)
                        if selection_applied:
                            break
                        last_selection_error = AssertionError(
                            f'Selection was rejected or expired; status remained {attempt_status_text!r}'
                        )
                    time.sleep(0.05)
                if not selection_applied:
                    raise AssertionError(
                        f'No fresh candidate selection was accepted after {selection_attempts} attempts '
                        f'and {candidate_visible_polls} candidate-visible polls; '
                        f'last candidate widget: {last_candidates_widget!r}; '
                        f'last result: {last_selection_error!s}'
                    )
            except AssertionError as selection_error:
                widgets = ui.request('widget-list', {}).get('children', [])
                status_widget = self._find_ui_widget(widgets, 'FaithfulFrame tracking status')
                candidates_widget = self._find_ui_widget(widgets, 'Detected face candidates')
                screenshot_path = None
                try:
                    screenshot = client.send('GetSourceScreenshot', {
                        'sourceName': camera_name,
                        'imageFormat': 'png',
                        'imageWidth': 320,
                        'imageHeight': 360,
                    }).image_data
                    if screenshot:
                        encoded = screenshot.partition(',')[2] or screenshot
                        logsdir = os.environ.get('ONSDRIVER_LOGS', 'logs')
                        os.makedirs(logsdir, exist_ok=True)
                        screenshot_path = os.path.join(logsdir, f'{camera_name.replace(" ", "_")}.png')
                        with open(screenshot_path, 'wb') as screenshot_file:
                            screenshot_file.write(base64.b64decode(encoded))
                except Exception as screenshot_error:
                    screenshot_path = f'unavailable: {screenshot_error}'
                self.fail(
                    f'{selection_error}\n'
                    f'Tracking status widget: {status_widget!r}\n'
                    f'Candidate list widget: {candidates_widget!r}\n'
                    f'Synthetic source screenshot: {screenshot_path!r}'
                )
            self._wait_for_ui_status(ui, 'Following the selected speaker', timeout=10.0)
            self._wait_for_ui_widget(ui, 'Tracking state: Locked')
            self._wait_for_ui_zoom_above(ui, 1.0, timeout=10.0)
            camera_after = self._wait_for_screenshot(
                client, camera_name, lambda image: image == camera_before, timeout=5.0
            )
            self.assertEqual(
                camera_after,
                camera_before,
                'Tracking must leave the original camera image unchanged',
            )
            adapter_after = self._wait_for_screenshot(
                client,
                adapter_name,
                lambda image: bool(image) and image != adapter_before,
                timeout=10.0,
            )

            # A scene deactivation must invalidate a previously locked target.
            # Returning to the same source must hold its last valid framing
            # and require a new operator selection before following resumes.
            color_kinds = [
                kind
                for kind in kinds_response.input_kinds
                if kind == 'color_source' or kind.startswith('color_source_v')
            ]
            self.assertTrue(color_kinds, 'OBS color source is required for scene-switch testing')
            color_kind = max(
                color_kinds,
                key=lambda kind: int(kind.rsplit('_v', 1)[1]) if '_v' in kind else 0,
            )
            client.send('CreateScene', {'sceneName': fallback_scene})
            fallback_scene_created = True
            client.send('CreateInput', {
                'sceneName': fallback_scene,
                'inputName': fallback_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF00FF00, 'width': 320, 'height': 180},
            })
            created_inputs.append(fallback_name)
            client.send('SetCurrentProgramScene', {'sceneName': fallback_scene})
            fallback_image = self._wait_for_screenshot(client, fallback_name, bool)
            inactive_adapter = self._wait_for_screenshot(
                client,
                adapter_name,
                lambda image: bool(image) and image != camera_before,
            )
            self.assertNotEqual(inactive_adapter, camera_before)
            self._wait_for_ui_status(ui, 'Camera inactive; activate its scene')
            self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=False)
            self.assertEqual(self._screenshot(client, fallback_name), fallback_image)

            client.send('SetCurrentProgramScene', {'sceneName': tracked_scene})
            camera_after_scene_return = self._wait_for_screenshot(
                client, camera_name, lambda image: image == camera_before
            )
            self.assertEqual(camera_after_scene_return, camera_before)
            scene_return_status = self._wait_for_ui_status(
                ui, 'Camera or tracking settings changed; select the speaker again before following'
            )
            self._wait_for_ui_widget(ui, 'Tracking state: Manual')
            zoom_text = scene_return_status.split('Current zoom: ', 1)[1].split('×', 1)[0]
            self.assertGreater(
                float(zoom_text),
                1.0,
                'Scene reactivation must hold the prior cropped framing instead of resuming wide',
            )
            # Selection is valid directly from Manual. Use the first fresh
            # candidate window after reactivation; do not spend its short TTL
            # waiting through a separate auto-follow/rearm transition.
            reselection_deadline = time.monotonic() + 30.0
            reselection_applied = False
            while time.monotonic() < reselection_deadline:
                widget_tree = ui.request('widget-list', {})
                widgets = widget_tree.get('children', [])
                if self._find_ui_widget(widgets, 'Tracking state: Locked'):
                    reselection_applied = True
                    break
                candidates_widget = self._find_ui_widget(widgets, 'Detected face candidates')
                # Only candidates visible in the current held crop are shown;
                # one visible speaker plus the placeholder is enough here.
                if candidates_widget and candidates_widget.get('count', 0) >= 2:
                    try:
                        self._invoke_ui_widget_from_tree(
                            ui,
                            widget_tree,
                            'accessibleName',
                            'Detected face candidates',
                            'setCurrentIndex',
                            arg1=1,
                        )
                        self._invoke_ui_widget_from_tree(
                            ui,
                            widget_tree,
                            'accessibleName',
                            'Select the highlighted face as the speaker',
                            'click',
                        )
                    except AssertionError:
                        time.sleep(0.05)
                        continue
                    lock_deadline = min(time.monotonic() + 1.0, reselection_deadline)
                    while time.monotonic() < lock_deadline:
                        lock_widgets = ui.request('widget-list', {}).get('children', [])
                        if self._find_ui_widget(lock_widgets, 'Tracking state: Locked'):
                            reselection_applied = True
                            break
                        time.sleep(0.05)
                    if reselection_applied:
                        break
                time.sleep(0.05)
            self.assertTrue(
                reselection_applied,
                'Returning to the tracked scene must require a fresh candidate selection',
            )
            self._wait_for_ui_status(ui, 'Following the selected speaker')
            self._wait_for_ui_zoom_above(ui, 1.0, timeout=10.0)
            adapter_after_scene_reselection = self._wait_for_screenshot(
                client,
                adapter_name,
                lambda image: screenshot_has_visible_pixels(image)
                and not screenshots_match_with_render_tolerance(
                    camera_after_scene_return, image
                ),
            )
            self.assertTrue(
                screenshot_has_visible_pixels(adapter_after_scene_reselection)
                and not screenshots_match_with_render_tolerance(
                    camera_after_scene_return, adapter_after_scene_reselection
                ),
                'A fresh selection after scene reactivation must render only a visible adapter crop',
            )
            self.assertEqual(self._screenshot(client, fallback_name), fallback_image)

            # Start from a genuinely locked, detector-confirmed target, then
            # clear the synthetic image input. This exercises camera-loss
            # invalidation through OBS callbacks, the serialized Rust session,
            # the dock, and the protected adapter output.
            client.send('SetInputSettings', {
                'inputName': camera_name,
                'inputSettings': {'file': ''},
                'overlay': True,
            })
            disconnected_settings = client.send('GetInputSettings', {'inputName': camera_name})
            self.assertEqual(disconnected_settings.input_settings.get('file'), '')
            self._wait_for_ui_status(
                ui, 'Camera disconnected; restore the camera, then select the speaker again'
            )
            self._wait_for_ui_widget(ui, 'Tracking status: attention required')
            self._wait_for_ui_widget(
                ui, 'Enable automatic speaker following', enabled=False
            )
            disconnected_output = self._wait_for_screenshot(
                client,
                adapter_name,
                lambda image: bool(image) and image not in (camera_before, adapter_after),
                timeout=10.0,
            )
            self.assertNotEqual(disconnected_output, camera_before)

            client.send('SetInputSettings', {
                'inputName': camera_name,
                'inputSettings': {'file': image_path},
                'overlay': True,
            })
            restored_settings = client.send('GetInputSettings', {'inputName': camera_name})
            self.assertEqual(restored_settings.input_settings.get('file'), image_path)
            self._wait_for_ui_status(
                ui, 'Camera or tracking settings changed; select the speaker again before following'
            )
            self._wait_for_ui_widget(ui, 'Tracking state: Manual')
            self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=True)
            camera_restored = self._wait_for_screenshot(
                client, camera_name, lambda image: image == camera_before
            )
            self.assertEqual(camera_restored, camera_before)
            adapter_restored = None
            restored_output_error = None
            try:
                adapter_restored = self._wait_for_screenshot(
                    client,
                    adapter_name,
                    lambda image: screenshots_match_with_render_tolerance(camera_restored, image),
                    timeout=10.0,
                )
            except AssertionError as error:
                restored_output_error = error
                adapter_restored = self._screenshot(client, adapter_name)
                logsdir = os.environ.get('ONSDRIVER_LOGS', 'logs')
                os.makedirs(logsdir, exist_ok=True)
                for label, screenshot_data in (
                    ('camera-restored', camera_restored),
                    ('adapter-after-reconnect', adapter_restored),
                    ('adapter-before-disconnect', adapter_after),
                    ('adapter-during-disconnect', disconnected_output),
                ):
                    encoded = screenshot_data.partition(',')[2] or screenshot_data
                    screenshot_path = os.path.join(logsdir, f'{label}-{suffix}.png')
                    with open(screenshot_path, 'wb') as screenshot_file:
                        screenshot_file.write(base64.b64decode(encoded))
            self.assertTrue(
                screenshots_match_with_render_tolerance(camera_restored, adapter_restored),
                'A restored camera must use wide framing until the operator selects again; '
                f'previous screenshot wait: {restored_output_error!s}',
            )

            self._invoke_ui_widget(
                ui, 'accessibleName', 'Enable automatic speaker following', 'click'
            )
            self._wait_for_ui_status(ui, 'No speaker selected; choose a visible face')
            self._wait_for_ui_widget(ui, 'Tracking state: Searching')
            time.sleep(0.5)
            self._wait_for_ui_widget(
                ui,
                'Tracking state: Searching',
            )

            # A live framing-config change must invalidate the selected target
            # and return only the adapter to safe-wide until the volunteer
            # explicitly selects a candidate again.
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_target_coverage_percent': 70.0},
                'overlay': True,
            })
            self._wait_for_ui_status(
                ui, 'Framing settings changed; rearm and select a speaker before following'
            )
            self._wait_for_ui_widget(ui, 'Tracking state: Manual')
            self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=True)
            updated_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertEqual(
                updated_settings.input_settings.get('safe_tracking_target_coverage_percent'),
                70.0,
            )
            camera_after_reconfiguration = self._wait_for_screenshot(
                client, camera_name, lambda image: image == camera_before
            )
            self.assertEqual(camera_after_reconfiguration, camera_before)
            adapter_after_reconfiguration = self._wait_for_screenshot(
                client,
                adapter_name,
                lambda image: screenshots_match_with_render_tolerance(
                    camera_after_reconfiguration, image
                ),
            )
            self.assertTrue(
                screenshots_match_with_render_tolerance(
                    camera_after_reconfiguration, adapter_after_reconfiguration
                ),
                'Framing reconfiguration must reset only the adapter to the original wide camera view',
            )
            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Enable automatic speaker following',
                'click',
            )
            self._wait_for_ui_status(ui, 'No speaker selected; choose a visible face')
            self._wait_for_ui_widget(ui, 'Tracking state: Searching')
            time.sleep(0.5)
            self._wait_for_ui_widget(
                ui,
                'Tracking state: Searching',
            )
            self.assertEqual(
                self._screenshot(client, camera_name),
                camera_before,
                'Rearming after framing changes must not alter the original camera feed',
            )
        finally:
            if dock_created:
                try:
                    self._invoke_ui_widget(ui, 'objectName', 'FTDock_ft_dock', 'removeDock')
                except Exception:
                    pass

            try:
                if original_scene:
                    client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            except Exception:
                pass

            try:
                names = self._input_names(client)
                for input_name in reversed(created_inputs):
                    if input_name in names:
                        client.send('RemoveInput', {'inputName': input_name})
            except Exception:
                pass

            if scene_created:
                try:
                    client.send('RemoveScene', {'sceneName': tracked_scene})
                except Exception:
                    pass
            if fallback_scene_created:
                try:
                    client.send('RemoveScene', {'sceneName': fallback_scene})
                except Exception:
                    pass

    def test_safe_adapter_scene_switch_holds_source_isolation(self):
        'Switch away from and back to a bound camera without changing either source'
        client = self.obs.get_obsws()
        ui = obsui.OBSUI(client)
        suffix = uuid.uuid4().hex
        tracked_scene = f'FaithfulFrame scene switch {suffix}'
        fallback_scene = f'FaithfulFrame fallback {suffix}'
        camera_name = f'FaithfulFrame tracked camera {suffix}'
        fallback_name = f'FaithfulFrame fallback camera {suffix}'
        adapter_name = f'FaithfulFrame scene adapter {suffix}'
        original_scene = None
        created_scenes = []
        created_inputs = []
        dock_created = False

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            color_kinds = [
                kind
                for kind in kinds_response.input_kinds
                if kind == 'color_source' or kind.startswith('color_source_v')
            ]
            self.assertTrue(color_kinds, 'OBS color source is required for hardware-free lifecycle testing')
            color_kind = max(
                color_kinds,
                key=lambda kind: int(kind.rsplit('_v', 1)[1]) if '_v' in kind else 0,
            )

            client.send('CreateScene', {'sceneName': tracked_scene})
            created_scenes.append(tracked_scene)
            client.send('SetCurrentProgramScene', {'sceneName': tracked_scene})
            client.send('CreateInput', {
                'sceneName': tracked_scene,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFFFF0000, 'width': 320, 'height': 180},
            })
            created_inputs.append(camera_name)
            camera_uuid = self._input_uuid(client, camera_name)
            client.send('CreateInput', {
                'sceneName': tracked_scene,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': camera_name,
                    'target_uuid': camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            created_inputs.append(adapter_name)

            camera_image = self._wait_for_screenshot(client, camera_name, bool)
            tracked_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: bool(image) and image == camera_image
            )
            self.assertEqual(tracked_image, camera_image)

            menu_tree = ui.menu_list().get('menu', [])
            dock_menu_path = self._find_menu_path(menu_tree, 'New FaithfulFrame Dock')
            self.assertIsNotNone(dock_menu_path, 'FaithfulFrame dock action must be available in OBS Tools')
            ui.request('menu-trigger', {'path': dock_menu_path})
            dock_created = True
            self._wait_for_ui_widget(
                ui, 'Camera source with FaithfulFrame tracking filter'
            )
            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Camera source with FaithfulFrame tracking filter',
                'setCurrentIndex',
                arg1=1,
            )
            self._wait_for_ui_status(ui, f'Tracking source: {camera_name}')
            self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=True)

            # The initial source-binding notice is valid until an operator
            # acts. Acknowledge it before isolating scene-switch messaging.
            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Reset framing to the full camera view',
                'click',
            )
            self._wait_for_ui_status(ui, 'Wide framing; select a face to follow again')

            client.send('CreateScene', {'sceneName': fallback_scene})
            created_scenes.append(fallback_scene)
            client.send('CreateInput', {
                'sceneName': fallback_scene,
                'inputName': fallback_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF00FF00, 'width': 320, 'height': 180},
            })
            created_inputs.append(fallback_name)
            client.send('SetCurrentProgramScene', {'sceneName': fallback_scene})
            fallback_image = self._wait_for_screenshot(client, fallback_name, bool)

            for cycle in range(3):
                inactive_image = self._wait_for_screenshot(
                    client, adapter_name, lambda image: bool(image) and image != camera_image
                )
                self.assertNotEqual(
                    inactive_image,
                    camera_image,
                    'An inactive scene must not keep rendering the bound camera through its adapter',
                )
                self._wait_for_ui_status(ui, 'Camera inactive; activate its scene')
                self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=False)
                self.assertEqual(
                    self._screenshot(client, fallback_name),
                    fallback_image,
                    'Scene switching must leave the fallback camera unchanged',
                )

                client.send('SetCurrentProgramScene', {'sceneName': tracked_scene})
                restored_image = self._wait_for_screenshot(
                    client, adapter_name, lambda image: bool(image) and image == camera_image
                )
                self.assertEqual(
                    restored_image,
                    camera_image,
                    'Returning to the bound scene must restore only its original camera view',
                )
                self._wait_for_ui_status(
                    ui, 'Camera or tracking settings changed; select the speaker again before following'
                )
                self._wait_for_ui_widget(ui, 'Tracking state: Manual')
                self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=True)
                self.assertEqual(self._screenshot(client, camera_name), camera_image)
                if cycle < 2:
                    client.send('SetCurrentProgramScene', {'sceneName': fallback_scene})

            self.assertEqual(self._screenshot(client, fallback_name), fallback_image)
        finally:
            if dock_created:
                try:
                    self._invoke_ui_widget(ui, 'objectName', 'FTDock_ft_dock', 'removeDock')
                except Exception:
                    pass

            try:
                if original_scene:
                    client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            except Exception:
                pass

            try:
                names = self._input_names(client)
                for input_name in created_inputs:
                    if input_name in names:
                        client.send('RemoveInput', {'inputName': input_name})
            except Exception:
                pass

            for scene_name in reversed(created_scenes):
                try:
                    client.send('RemoveScene', {'sceneName': scene_name})
                except Exception:
                    pass

    def test_safe_adapter_requires_explicit_rebinding_after_source_replacement(self):
        'Create a safe adapter and verify a same-name replacement stays unbound'
        client = self.obs.get_obsws()
        suffix = uuid.uuid4().hex
        scene_name = f'FaithfulFrame lifecycle {suffix}'
        camera_name = f'FaithfulFrame synthetic camera {suffix}'
        other_camera_name = f'FaithfulFrame unrelated camera {suffix}'
        adapter_name = f'FaithfulFrame safe adapter {suffix}'
        other_adapter_name = f'FaithfulFrame competing adapter {suffix}'
        original_scene = None
        scene_created = False

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            input_kinds = kinds_response.input_kinds
            color_kinds = [
                kind for kind in input_kinds if kind == 'color_source' or kind.startswith('color_source_v')
            ]
            self.assertTrue(color_kinds, 'OBS color source is required for hardware-free lifecycle testing')
            color_kind = max(
                color_kinds,
                key=lambda kind: int(kind.rsplit('_v', 1)[1]) if '_v' in kind else 0,
            )

            client.send('CreateScene', {'sceneName': scene_name})
            scene_created = True
            client.send('SetCurrentProgramScene', {'sceneName': scene_name})
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFFFF0000, 'width': 320, 'height': 180},
            })
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': other_camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF00FF00, 'width': 320, 'height': 180},
            })
            camera_uuid = self._input_uuid(client, camera_name)
            other_camera_uuid = self._input_uuid(client, other_camera_name)
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {'target_name': camera_name, 'target_uuid': camera_uuid},
            })
            adapter_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertEqual(adapter_settings.input_kind, 'face_tracker_source')
            self.assertEqual(adapter_settings.input_settings.get('target_name'), camera_name)
            self.assertEqual(adapter_settings.input_settings.get('target_uuid'), camera_uuid)
            self.assertFalse(adapter_settings.input_settings.get('safe_tracking_enabled', False))

            camera_image = self._wait_for_screenshot(client, camera_name, bool)
            other_camera_image = self._wait_for_screenshot(client, other_camera_name, bool)
            legacy_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == camera_image
            )
            self.assertEqual(legacy_image, camera_image)

            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': True},
                'overlay': True,
            })
            adapter_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertTrue(adapter_settings.input_settings.get('safe_tracking_enabled'))
            self.assertEqual(adapter_settings.input_settings.get('target_uuid'), camera_uuid)
            bound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == camera_image
            )

            # Establish the first adapter's lease before attempting a competing
            # one, so this assertion does not depend on OBS tick ordering.
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': other_adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': other_camera_name,
                    'target_uuid': other_camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            self._wait_for_log_text(
                'FaithfulFrame allows safe tracking on one source adapter at a time'
            )
            self._wait_for_screenshot(
                client, other_adapter_name, lambda image: image != other_camera_image
            )
            for _ in range(3):
                self.assertNotEqual(
                    self._screenshot(client, other_adapter_name),
                    other_camera_image,
                    'A competing safe adapter must not acquire the other camera',
                )
                retained_image = self._wait_for_screenshot(
                    client, adapter_name, lambda image: image == bound_image
                )
                self.assertEqual(retained_image, bound_image)
                time.sleep(0.1)

            client.send('RemoveInput', {'inputName': camera_name})
            self._wait_for_input_presence(client, camera_name, False)
            unavailable_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image != bound_image
            )

            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF0000FF, 'width': 320, 'height': 180},
            })
            replacement_image = self._wait_for_screenshot(
                client, camera_name, lambda image: image != camera_image
            )
            replacement_uuid = self._input_uuid(client, camera_name)
            self.assertNotEqual(replacement_uuid, camera_uuid)
            self.assertNotEqual(replacement_image, camera_image)
            for _ in range(3):
                self.assertEqual(self._screenshot(client, adapter_name), unavailable_image)
                self.assertEqual(self._screenshot(client, other_camera_name), other_camera_image)
                time.sleep(0.1)

            client.send('RemoveInput', {'inputName': other_adapter_name})
            self._wait_for_input_presence(client, other_adapter_name, False)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'target_name': other_camera_name, 'target_uuid': other_camera_uuid},
                'overlay': True,
            })
            other_rebound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == other_camera_image
            )
            self.assertEqual(other_rebound_image, other_camera_image)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'target_name': camera_name, 'target_uuid': replacement_uuid},
                'overlay': True,
            })
            rebound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == replacement_image
            )
            self.assertEqual(rebound_image, replacement_image)
            self.assertEqual(
                self._screenshot(client, camera_name),
                replacement_image,
                'Safe tracking must leave the original camera feed unchanged',
            )
            rebound_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertTrue(rebound_settings.input_settings.get('safe_tracking_enabled'))
            self.assertEqual(rebound_settings.input_settings.get('target_name'), camera_name)

            client.send('RemoveInput', {'inputName': adapter_name})
            self._wait_for_input_presence(client, adapter_name, False)
            self.assertIn(
                camera_name,
                self._input_names(client),
                'Removing the adapter must leave its camera intact',
            )
            self.assertEqual(
                self._screenshot(client, camera_name),
                replacement_image,
                'Removing the adapter must preserve the original camera feed',
            )
            self.assertEqual(self._screenshot(client, other_camera_name), other_camera_image)
        finally:
            # Keep this test isolated from the next OBS test even when an assertion fails.
            names = self._input_names(client)
            for input_name in (adapter_name, other_adapter_name, camera_name, other_camera_name):
                if input_name in names:
                    client.send('RemoveInput', {'inputName': input_name})
                    names.remove(input_name)
            if original_scene:
                client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            if scene_created:
                client.send('RemoveScene', {'sceneName': scene_name})

    def test_safe_adapter_resolution_change_requires_new_target_selection(self):
        'Invalidate camera geometry and return the safe adapter to wide framing'
        client = self.obs.get_obsws()
        ui = obsui.OBSUI(client)
        suffix = uuid.uuid4().hex
        scene_name = f'FaithfulFrame resolution change {suffix}'
        camera_name = f'FaithfulFrame resolution camera {suffix}'
        adapter_name = f'FaithfulFrame resolution adapter {suffix}'
        original_scene = None
        scene_created = False
        created_inputs = []
        dock_created = False

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            color_kinds = [
                kind
                for kind in kinds_response.input_kinds
                if kind == 'color_source' or kind.startswith('color_source_v')
            ]
            self.assertTrue(color_kinds, 'OBS color source is required for hardware-free geometry testing')
            color_kind = max(
                color_kinds,
                key=lambda kind: int(kind.rsplit('_v', 1)[1]) if '_v' in kind else 0,
            )

            client.send('CreateScene', {'sceneName': scene_name})
            scene_created = True
            client.send('SetCurrentProgramScene', {'sceneName': scene_name})
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF4A76C4, 'width': 320, 'height': 180},
            })
            created_inputs.append(camera_name)
            camera_uuid = self._input_uuid(client, camera_name)
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': camera_name,
                    'target_uuid': camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            created_inputs.append(adapter_name)

            camera_before = self._wait_for_screenshot(client, camera_name, bool)
            self.assertEqual(
                self._wait_for_screenshot(
                    client, adapter_name, lambda image: image == camera_before
                ),
                camera_before,
                'A newly bound safe adapter must start with the complete camera view',
            )

            menu_tree = ui.menu_list().get('menu', [])
            dock_menu_path = self._find_menu_path(menu_tree, 'New FaithfulFrame Dock')
            self.assertIsNotNone(dock_menu_path, 'FaithfulFrame dock action must be available in OBS Tools')
            ui.request('menu-trigger', {'path': dock_menu_path})
            dock_created = True
            source_selector_name = 'Camera source with FaithfulFrame tracking filter'
            self._wait_for_ui_widget(ui, source_selector_name)
            self._invoke_ui_widget(
                ui, 'accessibleName', source_selector_name, 'setCurrentIndex', arg1=1
            )
            self._wait_for_ui_status(ui, f'Tracking source: {camera_name}')
            self._wait_for_ui_widget(ui, 'Tracking state: Manual')

            # Clear the initial binding notice so the next message identifies
            # the camera geometry lifecycle event.
            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Reset framing to the full camera view',
                'click',
            )
            self._wait_for_ui_status(ui, 'Wide framing; select a face to follow again')

            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Enable automatic speaker following',
                'click',
            )
            self._wait_for_ui_status(ui, 'No speaker selected; choose a visible face')
            self._wait_for_ui_widget(ui, 'Tracking state: Searching')

            # Keep the aspect ratio constant so screenshot equality checks the
            # full-source fallback without conflating it with output padding.
            client.send('SetInputSettings', {
                'inputName': camera_name,
                'inputSettings': {'width': 640, 'height': 360},
                'overlay': True,
            })
            updated_settings = client.send('GetInputSettings', {'inputName': camera_name})
            self.assertEqual(updated_settings.input_settings.get('width'), 640)
            self.assertEqual(updated_settings.input_settings.get('height'), 360)

            self._wait_for_ui_status(
                ui,
                'Camera or tracking settings changed; select the speaker again before following',
            )
            self._wait_for_ui_widget(ui, 'Tracking state: Manual')
            self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=True)
            camera_after = self._wait_for_screenshot(client, camera_name, bool)
            adapter_after = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == camera_after
            )
            self.assertEqual(
                camera_after,
                camera_before,
                'Changing source geometry must not modify the original camera feed',
            )
            self.assertEqual(
                adapter_after,
                camera_after,
                'A geometry change must return only the protected adapter to full-source framing',
            )

            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Enable automatic speaker following',
                'click',
            )
            self._wait_for_ui_status(ui, 'No speaker selected; choose a visible face')
            self._wait_for_ui_widget(ui, 'Tracking state: Searching')
        finally:
            if dock_created:
                try:
                    self._invoke_ui_widget(ui, 'objectName', 'FTDock_ft_dock', 'removeDock')
                except Exception:
                    pass

            try:
                if original_scene:
                    client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            except Exception:
                pass

            try:
                names = self._input_names(client)
                for input_name in reversed(created_inputs):
                    if input_name in names:
                        client.send('RemoveInput', {'inputName': input_name})
            except Exception:
                pass

            if scene_created:
                try:
                    client.send('RemoveScene', {'sceneName': scene_name})
                except Exception:
                    pass

    def test_safe_adapter_requires_reselection_after_camera_signal_loss(self):
        'Hold safely while the bound camera signal is lost and restored'
        client = self.obs.get_obsws()
        ui = obsui.OBSUI(client)
        suffix = uuid.uuid4().hex
        scene_name = f'FaithfulFrame signal recovery {suffix}'
        camera_name = f'FaithfulFrame signal camera {suffix}'
        adapter_name = f'FaithfulFrame signal adapter {suffix}'
        original_scene = None
        scene_created = False
        created_inputs = []
        dock_created = False
        temporary = tempfile.TemporaryDirectory(prefix='faithfulframe-signal-')
        # Keep the image alive until OBS exits even if a Windows runner still
        # has the source decoder open during unittest cleanup.
        self.addCleanup(temporary.cleanup)
        image_path = os.path.join(temporary.name, 'camera-signal.png')
        write_synthetic_signal_png(image_path)

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            self.assertIn(
                'image_source',
                kinds_response.input_kinds,
                'OBS Image Source is required for hardware-free signal testing',
            )

            client.send('CreateScene', {'sceneName': scene_name})
            scene_created = True
            client.send('SetCurrentProgramScene', {'sceneName': scene_name})
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': camera_name,
                'inputKind': 'image_source',
                'inputSettings': {'file': image_path, 'unload': False},
            })
            created_inputs.append(camera_name)
            camera_uuid = self._input_uuid(client, camera_name)
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': camera_name,
                    'target_uuid': camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            created_inputs.append(adapter_name)

            camera_before = self._wait_for_screenshot(client, camera_name, bool)
            self.assertEqual(
                self._wait_for_screenshot(
                    client, adapter_name, lambda image: image == camera_before
                ),
                camera_before,
                'The bound adapter must initially show its camera',
            )

            menu_tree = ui.menu_list().get('menu', [])
            dock_menu_path = self._find_menu_path(menu_tree, 'New FaithfulFrame Dock')
            self.assertIsNotNone(dock_menu_path, 'FaithfulFrame dock action must be available in OBS Tools')
            ui.request('menu-trigger', {'path': dock_menu_path})
            dock_created = True
            source_selector_name = 'Camera source with FaithfulFrame tracking filter'
            self._wait_for_ui_widget(ui, source_selector_name)
            self._invoke_ui_widget(
                ui, 'accessibleName', source_selector_name, 'setCurrentIndex', arg1=1
            )
            self._wait_for_ui_status(ui, f'Tracking source: {camera_name}')
            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Reset framing to the full camera view',
                'click',
            )
            self._wait_for_ui_status(ui, 'Wide framing; select a face to follow again')
            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Enable automatic speaker following',
                'click',
            )
            self._wait_for_ui_status(ui, 'No speaker selected; choose a visible face')
            self._wait_for_ui_widget(ui, 'Tracking state: Searching')

            # Clearing an OBS Image Source removes its decoded dimensions,
            # exercising the same zero-size signal-loss path as a disconnected
            # camera without requiring capture hardware.
            client.send('SetInputSettings', {
                'inputName': camera_name,
                'inputSettings': {'file': ''},
                'overlay': True,
            })
            disconnected_settings = client.send('GetInputSettings', {'inputName': camera_name})
            self.assertEqual(disconnected_settings.input_settings.get('file'), '')
            self._wait_for_ui_status(
                ui, 'Camera disconnected; restore the camera, then select the speaker again'
            )
            self._wait_for_ui_widget(ui, 'Tracking status: attention required')
            self._wait_for_ui_widget(
                ui, 'Enable automatic speaker following', enabled=False
            )
            self.assertNotEqual(
                self._wait_for_screenshot(
                    client, adapter_name, lambda image: bool(image) and image != camera_before
                ),
                camera_before,
                'The adapter must fail closed instead of rendering a stale camera frame',
            )

            client.send('SetInputSettings', {
                'inputName': camera_name,
                'inputSettings': {'file': image_path},
                'overlay': True,
            })
            restored_settings = client.send('GetInputSettings', {'inputName': camera_name})
            self.assertEqual(restored_settings.input_settings.get('file'), image_path)
            self._wait_for_ui_status(
                ui,
                'Camera or tracking settings changed; select the speaker again before following',
            )
            self._wait_for_ui_widget(ui, 'Tracking state: Manual')
            self._wait_for_ui_widget(ui, 'Enable automatic speaker following', enabled=True)
            camera_restored = self._wait_for_screenshot(client, camera_name, bool)
            self.assertEqual(
                camera_restored,
                camera_before,
                'Camera signal recovery must not change the original source',
            )
            self.assertEqual(
                self._wait_for_screenshot(
                    client, adapter_name, lambda image: image == camera_restored
                ),
                camera_restored,
                'Signal restoration must put the adapter at safe-wide framing',
            )

            self._invoke_ui_widget(
                ui,
                'accessibleName',
                'Enable automatic speaker following',
                'click',
            )
            self._wait_for_ui_status(ui, 'No speaker selected; choose a visible face')
            self._wait_for_ui_widget(ui, 'Tracking state: Searching')
        finally:
            if dock_created:
                try:
                    self._invoke_ui_widget(ui, 'objectName', 'FTDock_ft_dock', 'removeDock')
                except Exception:
                    pass

            try:
                if original_scene:
                    client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            except Exception:
                pass

            try:
                names = self._input_names(client)
                for input_name in reversed(created_inputs):
                    if input_name in names:
                        client.send('RemoveInput', {'inputName': input_name})
            except Exception:
                pass

            if scene_created:
                try:
                    client.send('RemoveScene', {'sceneName': scene_name})
                except Exception:
                    pass

    def test_safe_adapter_stops_when_legacy_tracker_filter_is_added(self):
        'Fail closed if a legacy crop filter is added to the bound camera'
        client = self.obs.get_obsws()
        suffix = uuid.uuid4().hex
        scene_name = f'FaithfulFrame filter guard {suffix}'
        camera_name = f'FaithfulFrame filter guard camera {suffix}'
        adapter_name = f'FaithfulFrame filter guard adapter {suffix}'
        legacy_filter_name = f'FaithfulFrame legacy tracker {suffix}'
        original_scene = None
        scene_created = False
        filter_created = False

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            color_kinds = [
                kind
                for kind in kinds_response.input_kinds
                if kind == 'color_source' or kind.startswith('color_source_v')
            ]
            self.assertTrue(color_kinds, 'OBS color source is required for hardware-free lifecycle testing')
            color_kind = max(
                color_kinds,
                key=lambda kind: int(kind.rsplit('_v', 1)[1]) if '_v' in kind else 0,
            )

            client.send('CreateScene', {'sceneName': scene_name})
            scene_created = True
            client.send('SetCurrentProgramScene', {'sceneName': scene_name})
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF4A76C4, 'width': 320, 'height': 180},
            })
            camera_uuid = self._input_uuid(client, camera_name)
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': camera_name,
                    'target_uuid': camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            camera_image = self._wait_for_screenshot(client, camera_name, bool)
            bound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == camera_image
            )
            self.assertEqual(bound_image, camera_image)

            client.send('CreateSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
                'filterKind': 'face_tracker_filter',
                'filterSettings': {},
            })
            filter_created = True
            camera_with_legacy_filter = self._wait_for_screenshot(client, camera_name, bool)
            self._wait_for_log_text(
                'FaithfulFrame safe tracking stopped: remove the added legacy tracker filter, then rebind the source'
            )
            closed_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image != bound_image
            )
            self.assertNotEqual(
                closed_image,
                bound_image,
                'Adding a legacy crop filter must stop the protected output instead of applying its crop',
            )
            self.assertNotEqual(
                closed_image,
                camera_with_legacy_filter,
                'The stopped adapter must not pass through the newly filtered camera image',
            )

            client.send('RemoveSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
            })
            filter_created = False
            restored_camera_image = self._wait_for_screenshot(
                client, camera_name, lambda image: image == camera_image
            )
            self.assertEqual(restored_camera_image, camera_image)
            for _ in range(3):
                self.assertNotEqual(
                    self._screenshot(client, adapter_name),
                    bound_image,
                    'Removing the filter must not silently reopen the closed binding',
                )
                time.sleep(0.1)

            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': False},
                'overlay': True,
            })
            disabled_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(disabled_image, restored_camera_image)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': True},
                'overlay': True,
            })
            rebound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(rebound_image, restored_camera_image)

            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': False},
                'overlay': True,
            })
            disabled_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(disabled_image, restored_camera_image)
            # The adapter switches back to legacy rendering as soon as the
            # setting is applied; let the OBS video tick finish retiring the
            # old safe binding before testing a pre-existing filter.
            time.sleep(0.25)

            # Existing disabled filters remain incompatible: enabling safety
            # must not bind until the filter is removed and the operator retries.
            client.send('CreateSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
                'filterKind': 'face_tracker_filter',
                'filterSettings': {},
            })
            filter_created = True
            client.send('SetSourceFilterEnabled', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
                'filterEnabled': False,
            })
            filter_info = client.send('GetSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
            })
            self.assertFalse(filter_info.filter_enabled)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': True},
                'overlay': True,
            })
            self._wait_for_log_text('FaithfulFrame rejected an invalid, filtered, or unavailable source binding')
            rejected_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image != restored_camera_image
            )
            self.assertNotEqual(
                rejected_image,
                restored_camera_image,
                'A pre-existing disabled tracker filter must reject the safe source binding',
            )

            client.send('RemoveSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
            })
            filter_created = False
            self.assertEqual(self._screenshot(client, camera_name), restored_camera_image)
            for _ in range(3):
                self.assertNotEqual(
                    self._screenshot(client, adapter_name),
                    restored_camera_image,
                    'Removing an existing filter must not silently create a safe binding',
                )
                time.sleep(0.1)

            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': False},
                'overlay': True,
            })
            disabled_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(disabled_image, restored_camera_image)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': True},
                'overlay': True,
            })
            rebound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(rebound_image, restored_camera_image)
        finally:
            names = self._input_names(client)
            if filter_created and camera_name in names:
                try:
                    client.send('RemoveSourceFilter', {
                        'sourceName': camera_name,
                        'filterName': legacy_filter_name,
                    })
                except Exception:
                    pass
            for input_name in (adapter_name, camera_name):
                if input_name in names:
                    client.send('RemoveInput', {'inputName': input_name})
                    names.remove(input_name)
            if original_scene:
                client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            if scene_created:
                client.send('RemoveScene', {'sceneName': scene_name})


class ScreenshotPayloadValidationTest(unittest.TestCase):
    def test_empty_image_data_is_rejected(self):
        client = Mock()
        client.send.return_value = SimpleNamespace(image_data='')

        with self.assertRaisesRegex(AssertionError, 'empty screenshot'):
            PluginLoadTest._screenshot(None, client, 'camera')

    def test_nonempty_image_data_is_returned(self):
        client = Mock()
        client.send.return_value = SimpleNamespace(image_data='cG5n')

        self.assertEqual(PluginLoadTest._screenshot(None, client, 'camera'), 'cG5n')


if __name__ == '__main__':
    unittest.main()
