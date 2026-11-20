"""Regression checks for workflow token scope and action pinning."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"
SHA_PIN = re.compile(r"^[0-9a-f]{40}$")


def workflow_files():
    return sorted((*WORKFLOWS.glob("*.yml"), *WORKFLOWS.glob("*.yaml")))


def uses_references(text):
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = re.match(r"^\s*uses:\s*([^\s#]+)", line)
        if match:
            yield line_number, match.group(1)


def top_level_permissions(text):
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line == "permissions:":
            permissions = {}
            for entry in lines[index + 1 :]:
                if not entry.strip() or entry.lstrip().startswith("#"):
                    continue
                if not entry.startswith(" "):
                    break
                match = re.fullmatch(r"  ([A-Za-z0-9_-]+):\s*(\S+)\s*", entry)
                if match is None:
                    raise ValueError(f"unsupported permissions syntax: {entry!r}")
                permissions[match.group(1)] = match.group(2)
            return permissions
    return None


class GitHubActionsSecurityTests(unittest.TestCase):
    def test_each_workflow_has_read_only_repository_permissions(self):
        files = workflow_files()
        self.assertTrue(files, "no GitHub Actions workflow files were found")

        for path in files:
            with self.subTest(workflow=path.name):
                text = path.read_text(encoding="utf-8")
                permission_declarations = [
                    line for line in text.splitlines()
                    if re.fullmatch(r"\s*permissions:\s*", line)
                ]
                self.assertEqual(
                    ["permissions:"],
                    permission_declarations,
                    "permission scope must be declared once at workflow level",
                )
                self.assertEqual(
                    {"contents": "read"},
                    top_level_permissions(text),
                    "workflows must explicitly grant only read access to repository contents",
                )

    def test_external_actions_are_pinned_to_full_commit_shas(self):
        files = workflow_files()
        references = []
        for path in files:
            for line_number, reference in uses_references(
                path.read_text(encoding="utf-8")
            ):
                if reference.startswith("./"):
                    continue
                references.append((path.name, line_number, reference))

        self.assertTrue(references, "no external GitHub Actions references were found")
        for workflow, line_number, reference in references:
            with self.subTest(workflow=workflow, line=line_number, action=reference):
                action, separator, revision = reference.rpartition("@")
                self.assertTrue(action, "action reference has no action name")
                self.assertEqual("@", separator, "action reference has no pinned revision")
                self.assertRegex(
                    revision,
                    SHA_PIN,
                    "external actions must use a full 40-character commit SHA",
                )


if __name__ == "__main__":
    unittest.main()
