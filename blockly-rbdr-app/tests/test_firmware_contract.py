from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUTTON_MODULE = REPO_ROOT / "button-module"


def read_button_module(path: str) -> str:
    return (BUTTON_MODULE / path).read_text(encoding="utf-8")


def test_button_firmware_polls_with_module_query():
    source = read_button_module("main/app.cpp")

    assert 'constexpr char kModuleName[] = "button";' in source
    assert '"%s/poll?module=%s", RBDR_SERVER_BASE_URL, kModuleName' in source


def test_button_firmware_identity_names_are_not_rebounder():
    checked_files = [
        "CMakeLists.txt",
        "main/app.cpp",
        "main/app.h",
        "main/main.cpp",
    ]

    for relative_path in checked_files:
        assert "rebounder" not in read_button_module(relative_path)
