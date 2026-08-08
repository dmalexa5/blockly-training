from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUTTON_MODULE = REPO_ROOT / "button-module"
RBDR_MODULE = REPO_ROOT / "rbdr-module"


def read_button_module(path: str) -> str:
    return (BUTTON_MODULE / path).read_text(encoding="utf-8")


def read_rbdr_module(path: str) -> str:
    return (RBDR_MODULE / path).read_text(encoding="utf-8")


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


def test_rebounder_firmware_exists_as_independent_project():
    expected_files = [
        "CMakeLists.txt",
        "sdkconfig.defaults",
        "main/CMakeLists.txt",
        "main/idf_component.yml",
        "main/app.h",
        "main/main.cpp",
        "main/app.cpp",
    ]

    for relative_path in expected_files:
        assert (RBDR_MODULE / relative_path).is_file()

    assert "project(rebounder-module)" in read_rbdr_module("CMakeLists.txt")
    assert "void rebounder_app_start();" in read_rbdr_module("main/app.h")
    assert "rebounder_app_start();" in read_rbdr_module("main/main.cpp")


def test_rebounder_firmware_polls_with_module_query():
    source = read_rbdr_module("main/app.cpp")

    assert 'constexpr char kModuleName[] = "rebounder";' in source
    assert '"%s/poll?module=%s", RBDR_SERVER_BASE_URL, kModuleName' in source


def test_rebounder_firmware_identity_names_are_not_button():
    checked_files = [
        "CMakeLists.txt",
        "main/app.cpp",
        "main/app.h",
        "main/main.cpp",
    ]

    for relative_path in checked_files:
        assert "button" not in read_rbdr_module(relative_path)


def test_rebounder_firmware_acceleration_config_defaults():
    cmake = read_rbdr_module("main/CMakeLists.txt")

    assert "set(RBDR_ACCEL_THRESHOLD_G 2.0 " in cmake
    assert "set(RBDR_ACCEL_EMA_ALPHA 0.6 " in cmake
    assert "set(RBDR_MPU_SDA_GPIO 8 " in cmake
    assert "set(RBDR_MPU_SCL_GPIO 9 " in cmake
    assert "set(RBDR_MPU_I2C_ADDR 0x68 " in cmake
