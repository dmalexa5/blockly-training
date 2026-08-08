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


def test_button_firmware_configures_active_low_pullup_gpio():
    source = read_button_module("main/app.cpp")
    cmake = read_button_module("main/CMakeLists.txt")

    assert 'set(RBDR_BUTTON_GPIO 4 CACHE STRING "INPUT_PULLUP button GPIO")' in cmake
    assert "io_conf.mode = GPIO_MODE_INPUT;" in source
    assert "io_conf.pull_up_en = GPIO_PULLUP_ENABLE;" in source
    assert "io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;" in source
    assert "gpio_get_level(static_cast<gpio_num_t>(RBDR_BUTTON_GPIO)) == 0" in source
    assert "wiring=GPIO%d-to-GND" in source


def test_button_firmware_logs_button_press_and_release_transitions():
    source = read_button_module("main/app.cpp")

    assert '"button transition event=%s gpio=%d raw_level=%d pressed=%s uptime_ms=%" PRIu32' in source
    assert 'fell ? "button_press" : "button_release"' in source


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


def test_rebounder_firmware_latches_edge_until_control_task_consumes_it():
    source = read_rbdr_module("main/app.cpp")

    assert "state.rising_edge = state.rising_edge || g_latest_state.rising_edge;" in source
    assert "g_latest_state.rising_edge = false;" in source
