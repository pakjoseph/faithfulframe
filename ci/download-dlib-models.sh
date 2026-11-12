#! /bin/bash

set -euo pipefail

flg_nonfree=0
destination="${DESTDIR:-./}"
# Freeze the current upstream model assets so builds and performance baselines
# do not silently change when dlib-models/master moves.
dlib_models_commit="fd81b6308a6a73d4ce08859eb2f4b628a21e27a2"
hog_model_sha256="7ef0315b8a893808a8c8c1677c7f49b519538ed1b4205d6d240a0274effd2975"

while (($# > 0)); do
	case "$1" in
		--nonfree)
			flg_nonfree=1
			shift ;;
		*)
			echo "Error: unknown option $1" >&2
			exit 1;;
	esac
done

mkdir -p "${destination}/data/dlib_hog_model" \
	"${destination}/data/dlib_cnn_model" \
	"${destination}/data/dlib_face_landmark_model"

# A previous opt-in download must not leave restricted assets in a later
# default build, since packaging recursively includes the data directory.
if ((!flg_nonfree)); then
	rm -f \
		"${destination}/data/dlib_face_landmark_model/shape_predictor_68_face_landmarks.dat" \
		"${destination}/data/dlib_face_landmark_model/shape_predictor_68_face_landmarks_GTX.dat" \
		"${destination}/data/LICENSE-shape_predictor_68_face_landmarks"
fi

model_tmp="$(mktemp -d)"
trap 'rm -rf "$model_tmp"' EXIT

hog_archive="$model_tmp/frontal_face_detector.dat.bz2"
hog_license="$model_tmp/frontal_face_detector-LICENSE.txt"
curl --fail --location --retry 3 --silent --show-error \
	'https://github.com/norihiro/obs-face-tracker/releases/download/0.7.0-hogdata/frontal_face_detector.dat.bz2' \
	--output "$hog_archive"
curl --fail --location --retry 3 --silent --show-error \
	'https://github.com/norihiro/obs-face-tracker/releases/download/0.7.0-hogdata/LICENSE.txt' \
	--output "$hog_license"
if ! cmp -s "$hog_license" dlib/LICENSE.txt; then
	echo "HOG detector license differs from the bundled dlib license" >&2
	exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
	actual_hog_sha256="$(sha256sum "$hog_archive" | cut -d ' ' -f1)"
elif command -v shasum >/dev/null 2>&1; then
	actual_hog_sha256="$(shasum -a 256 "$hog_archive" | cut -d ' ' -f1)"
else
	echo "sha256sum or shasum is required to verify the HOG model" >&2
	exit 1
fi
if [[ "$actual_hog_sha256" != "$hog_model_sha256" ]]; then
	echo "HOG model checksum mismatch: expected $hog_model_sha256, got $actual_hog_sha256" >&2
	exit 1
fi

unpack_bzip2()
{
	local source_file="$1"
	local destination_file="$2"
	if command -v bunzip2 >/dev/null 2>&1; then
		bunzip2 < "$source_file" > "$destination_file"
	elif command -v 7z >/dev/null 2>&1; then
		7z x "$source_file" -so > "$destination_file"
	else
		echo "Neither bunzip2 nor 7z is available to unpack model files" >&2
		return 1
	fi
}

unpack_bzip2 "$hog_archive" "${destination}/data/dlib_hog_model/frontal_face_detector.dat"

git init -q "$model_tmp/dlib-models"
git -C "$model_tmp/dlib-models" remote add origin https://github.com/davisking/dlib-models.git
git -C "$model_tmp/dlib-models" -c protocol.version=2 fetch --filter=blob:none --depth=1 origin \
	"$dlib_models_commit"
model_files=(
	/LICENSE
	/README.md
	/mmod_human_face_detector.dat.bz2
	/shape_predictor_5_face_landmarks.dat.bz2
)
if ((flg_nonfree)); then
	model_files+=(
		/shape_predictor_68_face_landmarks.dat.bz2
		/shape_predictor_68_face_landmarks_GTX.dat.bz2
	)
fi
git -C "$model_tmp/dlib-models" sparse-checkout init --no-cone
printf '%s\n' "${model_files[@]}" | \
	git -C "$model_tmp/dlib-models" sparse-checkout set --no-cone --stdin
git -C "$model_tmp/dlib-models" checkout --quiet --detach FETCH_HEAD
if [[ "$(git -C "$model_tmp/dlib-models" rev-parse HEAD)" != "$dlib_models_commit" ]]; then
	echo "Checked out an unexpected dlib-models revision" >&2
	exit 1
fi

unpack_bzip2 "$model_tmp/dlib-models/mmod_human_face_detector.dat.bz2" \
	"${destination}/data/dlib_cnn_model/mmod_human_face_detector.dat"
unpack_bzip2 "$model_tmp/dlib-models/shape_predictor_5_face_landmarks.dat.bz2" \
	"${destination}/data/dlib_face_landmark_model/shape_predictor_5_face_landmarks.dat"
cp dlib/LICENSE.txt "${destination}/data/LICENSE-dlib"
cp "$model_tmp/dlib-models/LICENSE" "${destination}/data/LICENSE-dlib-models"

if ((flg_nonfree)); then
	unpack_bzip2 "$model_tmp/dlib-models/shape_predictor_68_face_landmarks.dat.bz2" \
		"${destination}/data/dlib_face_landmark_model/shape_predictor_68_face_landmarks.dat"
	unpack_bzip2 "$model_tmp/dlib-models/shape_predictor_68_face_landmarks_GTX.dat.bz2" \
		"${destination}/data/dlib_face_landmark_model/shape_predictor_68_face_landmarks_GTX.dat"
	awk '/^##/{p=0} /^##.*shape_predictor_68/{p=1} p' "$model_tmp/dlib-models/README.md" \
		> "${destination}/data/LICENSE-shape_predictor_68_face_landmarks"
fi
