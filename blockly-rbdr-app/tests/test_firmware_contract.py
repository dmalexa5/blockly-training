from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE = REPO_ROOT / "module"


def read_module(path: str) -> str:
    return (MODULE / path).read_text(encoding="utf-8")


def test_single_module_project_replaces_old_layout():
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
        assert (MODULE / relative_path).is_file()

    assert not (REPO_ROOT / "button-module").exists()
    assert not (REPO_ROOT / "rbdr-module").exists()


def test_module_type_validation_and_module_name_compile_definition():
    root_cmake = read_module("CMakeLists.txt")
    component_cmake = read_module("main/CMakeLists.txt")

    assert 'set(MODULE_TYPE "" CACHE STRING "Module type: button or rebounder")' in root_cmake
    assert 'message(FATAL_ERROR "MODULE_TYPE must be set to' in root_cmake
    assert 'set(MODULE_NAME "" CACHE STRING "Logical module ID reported to the backend")' in component_cmake
    assert 'message(FATAL_ERROR "MODULE_NAME must be set")' in component_cmake
    assert 'MODULE_NAME="${MODULE_NAME}"' in component_cmake
    assert "RBDR_" not in component_cmake


def test_three_tasks_and_three_protected_state_structs():
    source = read_module("main/app.cpp")

    assert "struct state_desr_t" in source
    assert "struct state_sens_t" in source
    assert "struct state_ctrl_t" in source
    assert "SemaphoreHandle_t g_desr_mutex" in source
    assert "SemaphoreHandle_t g_sens_mutex" in source
    assert "SemaphoreHandle_t g_ctrl_mutex" in source
    assert 'xTaskCreate(webserver_task, "webserver"' in source
    assert 'xTaskCreate(sense_task, "sense"' in source
    assert 'xTaskCreate(control_task, "control"' in source


def test_hal_separates_raw_sensors_and_led_output():
    source = read_module("main/app.cpp")

    assert "int hal_read_button_gpio_level()" in source
    assert "gpio_get_level(static_cast<gpio_num_t>(BUTTON_GPIO))" in source
    assert "esp_err_t hal_read_mpu_accel(axis_sample_t *sample, mpu_t &mpu)" in source
    assert "i2c_master_transmit_receive(mpu.dev" in source
    assert "void hal_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)" in source
    assert "void hal_led_clear()" in source
    assert "sqrtf(raw_sample.x * raw_sample.x + raw_sample.y * raw_sample.y + raw_sample.z * raw_sample.z)" in source
    assert "const bool is_over_threshold = magnitude_g > ACCEL_THRESHOLD_G;" in source


def test_device_hosted_api_contracts_are_lean():
    source = read_module("main/app.cpp")

    assert 'health.uri = "/health";' in source
    assert 'command.uri = "/command";' in source
    assert '{\\"module\\":\\"%s\\",\\"type\\":\\"%s\\"}' in source
    assert 'httpd_resp_set_status(req, "202 Accepted");' in source
    assert 'httpd_resp_set_status(req, "409 Conflict");' in source
    assert "/poll" not in source


def test_backend_event_contract_stays_pending_until_ack():
    source = read_module("main/app.cpp")

    assert 'snprintf(url, sizeof(url), "%s/events", SERVER_BASE_URL)' in source
    assert '{\\"module\\":\\"%s\\",\\"event\\":\\"%s\\"}' in source
    assert "status >= 200 && status < 300" in source
    assert "desired.event_ack_pending = true;" in source
    assert "ctrl.event_pending = false;" in source


def test_preserved_button_and_rebounder_behavior():
    source = read_module("main/app.cpp")
    cmake = read_module("main/CMakeLists.txt")

    assert "set(DEFAULT_SENSE_PERIOD_MS 1)" in cmake
    assert "set(DEFAULT_SENSE_PERIOD_MS 10)" in cmake
    assert "fell ? \"button_press\" : \"button_release\"" in source
    assert "g_reset_sensor_filter.store(true);" in source
    assert "DEACTIVATE_IGNORE_MS" in source
    assert "ESP_LOGW(TAG, \"activate ignored: already_triggered\");" in source
    assert "hal_led_set_rgb(0, 0, 32);" in source
    assert "hal_led_set_rgb(32, 0, 0);" in source
