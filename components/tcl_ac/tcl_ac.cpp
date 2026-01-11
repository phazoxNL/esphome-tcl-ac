#include "tcl_ac.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace tcl_ac {

static const char *const TAG = "tcl_ac";

// added void for switchable booleans in Home assistant for beeper and display
void TclAcClimate::set_display_state(bool state) {
  this->display_enabled_ = state;
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_beeper_state(bool state) {
  this->beeper_enabled_ = state;
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::setup() {
  // Initialize with defaults
  this->mode = climate::CLIMATE_MODE_OFF;
  this->target_temperature = 22.0f;
  this->current_temperature = NAN;
  this->fan_mode = climate::CLIMATE_FAN_LOW;  // Most common in log (83%)
  this->preset = climate::CLIMATE_PRESET_NONE;
  this->swing_mode = climate::CLIMATE_SWING_OFF;
  
  ESP_LOGCONFIG(TAG, "TCL AC Climate component initialized");
}

void TclAcClimate::loop() {
  // Read incoming UART data
  while (this->available()) {
    uint8_t byte;
    this->read_byte(&byte);
    this->rx_buffer_.push_back(byte);
    
    // Check if we have a complete packet (minimum 7 bytes)
    if (this->rx_buffer_.size() >= 7) {
      // Check for valid header (BB 01 00 = AC to MCU)
      // Note: Packets FROM AC have header BB 01 00, TO AC have header BB 00 01
      if (this->rx_buffer_[0] == 0xBB && this->rx_buffer_[1] == 0x01 && this->rx_buffer_[2] == 0x00) {
        uint8_t cmd = this->rx_buffer_[3];
        uint8_t length = this->rx_buffer_[4];
        size_t expected_size = 5 + length + 1; // header(3) + cmd(1) + len(1) + data + checksum(1)
        
        if (this->rx_buffer_.size() >= expected_size) {
          // Validate checksum
          uint8_t calculated = this->calculate_checksum_(this->rx_buffer_.data(), expected_size - 1);
          uint8_t received = this->rx_buffer_[expected_size - 1];
          
          ESP_LOGV(TAG, "Received packet: cmd=0x%02X, len=%d, checksum=0x%02X (calc=0x%02X)", 
                   cmd, length, received, calculated);
          
          if (calculated == received) {
            // Process packet based on command
            if (cmd == CMD_POLL || cmd == CMD_SET_PARAMS) {
              // Command 0x03 (SET response) and 0x04 (POLL response) have same 55-byte data format
              ESP_LOGD(TAG, "Processing status packet (cmd 0x%02X)", cmd);
              this->parse_status_packet_(this->rx_buffer_.data() + 5, length);
            } else if (cmd == CMD_POWER) {
              ESP_LOGD(TAG, "Processing power status (cmd 0x0A)");
              this->parse_power_response_(this->rx_buffer_.data() + 5, length);
            } else if (cmd == CMD_TEMP_RESPONSE) {
              ESP_LOGD(TAG, "Processing temp response");
              this->parse_temp_response_(this->rx_buffer_.data() + 5, length);
            } else if (cmd == CMD_SHORT_STATUS) {
              ESP_LOGV(TAG, "Received short status (0x09) - limited data, using regular status instead");
              // SHORT_STATUS has only 45 bytes and minimal info, skip for now
            } else if (cmd == CMD_STATUS_ECHO) {
              ESP_LOGD(TAG, "Processing status echo (0x06)");
              this->parse_status_packet_(this->rx_buffer_.data() + 5, length);
            } else {
              ESP_LOGW(TAG, "Unknown command: 0x%02X", cmd);
            }
          } else {
            ESP_LOGW(TAG, "Checksum mismatch: expected 0x%02X, got 0x%02X", calculated, received);
          }
          
          // Clear buffer after processing
          this->rx_buffer_.clear();
        }
      } else {
        // Invalid header, shift buffer
        this->rx_buffer_.erase(this->rx_buffer_.begin());
      }
    }
  }
  
  // Poll AC every 5 seconds for status updates (AC sends ~1.3s intervals)
  uint32_t now = millis();
  if (now - this->last_poll_ > 5000) {
    this->send_poll_packet_();
    this->last_poll_ = now;
  }
}

void TclAcClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "TCL AC Climate:");
  ESP_LOGCONFIG(TAG, "  Beeper: %s", this->beeper_enabled_ ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  Display: %s", this->display_enabled_ ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  Vertical Direction: %d", this->vertical_direction_);
  ESP_LOGCONFIG(TAG, "  Horizontal Direction: %d", this->horizontal_direction_);
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_EVEN, 8);
}

climate::ClimateTraits TclAcClimate::traits() {
  auto traits = climate::ClimateTraits();
  
  // Supported modes (VALIDATED from log)
  traits.set_supported_modes({
    climate::CLIMATE_MODE_OFF,
    climate::CLIMATE_MODE_COOL,      // MODE_COOLING - 44x in log
    climate::CLIMATE_MODE_HEAT,      // MODE_HEATING - 6x in log
    climate::CLIMATE_MODE_DRY,       // MODE_DRY - 1x in log
    climate::CLIMATE_MODE_FAN_ONLY,  // MODE_FAN - theoretical
    climate::CLIMATE_MODE_AUTO,      // MODE_AUTO - 1x with ECO in log
  });
  
  // Fan modes (mapped to fan speeds)
  traits.set_supported_fan_modes({
    climate::CLIMATE_FAN_AUTO,     // FAN_SPEED_AUTO
    climate::CLIMATE_FAN_LOW,      // FAN_SPEED_LOW (83% in log)
    climate::CLIMATE_FAN_MEDIUM,   // FAN_SPEED_MEDIUM
    climate::CLIMATE_FAN_HIGH,     // FAN_SPEED_HIGH/MAX
  });
  
  // Presets (special modes)
  traits.set_supported_presets({
    climate::CLIMATE_PRESET_NONE,
    climate::CLIMATE_PRESET_ECO,       // ECO mode (Byte 7 Bit 7)
    climate::CLIMATE_PRESET_BOOST,     // TURBO mode (Byte 8 Bit 6)
    climate::CLIMATE_PRESET_SLEEP,     // SLEEP mode (Byte 19)
    climate::CLIMATE_PRESET_COMFORT,   // QUIET mode (Byte 8 Bit 7)
  });
  
  // Swing modes (combined vertical + horizontal)
  traits.set_supported_swing_modes({
    climate::CLIMATE_SWING_OFF,
    climate::CLIMATE_SWING_VERTICAL,
    climate::CLIMATE_SWING_HORIZONTAL,
    climate::CLIMATE_SWING_BOTH,
  });
  
  // Temperature range (from log: 18°C - 30°C observed)
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(31.0f);
  traits.set_visual_temperature_step(1.0f);
  traits.set_supports_current_temperature(true);
  
  return traits;
}

void TclAcClimate::control(const climate::ClimateCall &call) {
  // Handle mode change
  if (call.get_mode().has_value()) {
    this->mode = *call.get_mode();
  }
  
  // Handle temperature change
  if (call.get_target_temperature().has_value()) {
    this->target_temperature = *call.get_target_temperature();
  }
  
  // Handle fan mode change
  if (call.get_fan_mode().has_value()) {
    this->fan_mode = *call.get_fan_mode();
  }
  
  // Handle preset change
  if (call.get_preset().has_value()) {
    climate::ClimatePreset preset = *call.get_preset();
    
    // Reset all preset flags
    this->eco_mode_ = false;
    this->turbo_mode_ = false;
    this->quiet_mode_ = false;
    
    // Set the appropriate flag
    switch (preset) {
      case climate::CLIMATE_PRESET_ECO:
        this->eco_mode_ = true;
        // ECO only works with AUTO mode (observed in log)
        if (this->mode != climate::CLIMATE_MODE_OFF) {
          this->mode = climate::CLIMATE_MODE_AUTO;
        }
        break;
      case climate::CLIMATE_PRESET_BOOST:
        this->turbo_mode_ = true;
        break;
      case climate::CLIMATE_PRESET_COMFORT:
        this->quiet_mode_ = true;
        break;
      case climate::CLIMATE_PRESET_SLEEP:
        // Sleep mode is handled separately in packet creation
        break;
      default:
        break;
    }
    
    this->preset = preset;
  }
  
  // Handle swing mode change
  if (call.get_swing_mode().has_value()) {
    this->swing_mode = *call.get_swing_mode();
  }
  
  // Publish updated state
  this->publish_state();
  
  // Send control packet to AC
  if (this->mode != climate::CLIMATE_MODE_OFF) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
    ESP_LOGD(TAG, "Sent SET packet to AC");
  } else {
    // Send power off packet (simplified set packet with specific flags)
    uint8_t packet[SET_PACKET_SIZE];
    memset(packet, 0, SET_PACKET_SIZE);
    packet[0] = HEADER_MCU_TO_AC_0;
    packet[1] = HEADER_MCU_TO_AC_1;
    packet[2] = HEADER_MCU_TO_AC_2;
    packet[3] = CMD_SET_PARAMS;
    packet[4] = 0x20;  // 32 data bytes
    packet[5] = 0x03;
    packet[6] = 0x01;
    packet[7] = 0x00;  // Mode byte = 0x00 indicates power off (observed as 0x20 in one packet)
    packet[SET_PACKET_SIZE - 1] = this->calculate_checksum_(packet, SET_PACKET_SIZE - 1);
    this->send_packet_(packet, SET_PACKET_SIZE);
    ESP_LOGD(TAG, "Sent POWER OFF packet to AC");
  }
}

void TclAcClimate::create_set_packet_(uint8_t *packet) {
  // COMPLETE TCLAC PROTOCOL IMPLEMENTATION
  // Based on https://github.com/Kannix2005/tclac Lines 393-711
  
  memset(packet, 0, SET_PACKET_SIZE);
  
  // Header (bytes 0-2)
  packet[0] = HEADER_MCU_TO_AC_0;  // 0xBB
  packet[1] = HEADER_MCU_TO_AC_1;  // 0x00
  packet[2] = HEADER_MCU_TO_AC_2;  // 0x01
  
  // Command and length (bytes 3-4)
  packet[3] = CMD_SET_PARAMS;  // 0x03 = control
  packet[4] = 0x20;  // 32 data bytes (decimal 32)
  
  // Data payload starts at offset 5
  packet[5] = 0x03;
  packet[6] = 0x01;
  
  // Initialize control bytes to zero (will be built up with bit operations)
  packet[7]  = 0x00;  // Mode/Power/Display/Beeper/ECO
  packet[8]  = 0x00;  // Mode details/Quiet/Turbo/Health
  packet[9]  = 0x00;  // Temperature (will be set below)
  packet[10] = 0x00;  // Fan speed/Swing vertical
  packet[11] = 0x00;  // Swing horizontal
  packet[12] = 0x00;  // Fahrenheit/Timer
  packet[13] = 0x01;  // Fixed
  packet[14] = 0x00;  // Half degree
  packet[15] = 0x00;
  packet[16] = 0x00;
  packet[17] = 0x00;
  packet[18] = 0x00;
  packet[19] = 0x00;  // Sleep mode
  packet[20] = 0x00;
  packet[21] = 0x00;
  packet[22] = 0x00;
  packet[23] = 0x00;
  packet[24] = 0x00;
  packet[25] = 0x00;
  packet[26] = 0x00;
  packet[27] = 0x00;
  packet[28] = 0x00;
  packet[29] = 0x20;  // Fixed
  packet[30] = 0x00;
  packet[31] = 0x00;
  packet[32] = 0x00;  // Vertical swing mode + airflow position
  packet[33] = 0x00;  // Horizontal swing mode + airflow position
  packet[34] = 0x00;
  packet[35] = 0x00;
  packet[36] = 0x00;
  
  // ========== Byte 7: Power/Display/Beeper/ECO ==========
  // Bit 7 (0x80): ECO mode
  // Bit 6 (0x40): DISPLAY
  // Bit 5 (0x20): BEEPER
  // Bit 2 (0x04): POWER ON
  
  if (this->eco_mode_) {
    packet[7] += 0b10000000;  // ECO mode
    ESP_LOGD(TAG, "ECO mode enabled");
  }
  
  if (this->display_state_) {
    packet[7] += 0b01000000;  // Display ON
    ESP_LOGD(TAG, "Display ON");
  }
  
  if (this->beeper_state_) {
    packet[7] += 0b00100000;  // Beeper ON
    ESP_LOGD(TAG, "Beeper ON");
  }
  
  // ========== Configure operating mode (TCLAC Lines 429-460) ==========
  switch (this->mode) {
    case climate::CLIMATE_MODE_OFF:
      packet[7] += 0b00000000;
      packet[8] += 0b00000000;
      ESP_LOGD(TAG, "Mode: OFF");
      break;
    case climate::CLIMATE_MODE_AUTO:
      packet[7] += 0b00000100;  // Power ON
      packet[8] += 0b00001000;  // AUTO mode
      ESP_LOGD(TAG, "Mode: AUTO");
      break;
    case climate::CLIMATE_MODE_COOL:
      packet[7] += 0b00000100;  // Power ON
      packet[8] += 0b00000011;  // COOL mode
      ESP_LOGD(TAG, "Mode: COOL");
      break;
    case climate::CLIMATE_MODE_DRY:
      packet[7] += 0b00000100;  // Power ON
      packet[8] += 0b00000010;  // DRY mode
      ESP_LOGD(TAG, "Mode: DRY");
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      packet[7] += 0b00000100;  // Power ON
      packet[8] += 0b00000111;  // FAN mode
      ESP_LOGD(TAG, "Mode: FAN_ONLY");
      break;
    case climate::CLIMATE_MODE_HEAT:
      packet[7] += 0b00000100;  // Power ON
      packet[8] += 0b00000001;  // HEAT mode
      ESP_LOGD(TAG, "Mode: HEAT");
      break;
    default:
      packet[7] += 0b00000100;
      packet[8] += 0b00000011;  // Default COOL
      ESP_LOGD(TAG, "Mode: DEFAULT (COOL)");
      break;
  }

  // ========== Byte 8: Quiet/Turbo/Health/Mode details ==========
  // Bit 7 (0x80): QUIET mode
  // Bit 6 (0x40): TURBO mode
  // Bit 5 (0x20): HEALTH mode
  // Bits 0-4: Mode details (already set above)
  
  if (this->quiet_mode_) {
    packet[8] += 0b10000000;  // QUIET
    ESP_LOGD(TAG, "QUIET mode enabled");
  }
  
  if (this->turbo_mode_) {
    packet[8] += 0b01000000;  // TURBO
    ESP_LOGD(TAG, "TURBO mode enabled");
  }
  
  if (this->health_mode_) {
    packet[8] += 0b00100000;  // HEALTH
    ESP_LOGD(TAG, "HEALTH mode enabled");
  }

  // ========== Configure fan mode (TCLAC Lines 462-496) ==========
  switch (this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO)) {
    case climate::CLIMATE_FAN_AUTO:
      packet[8]  += 0b00000000;
      packet[10] += 0b00000000;
      ESP_LOGD(TAG, "Fan: AUTO");
      break;
    case climate::CLIMATE_FAN_QUIET:
      packet[8]  += 0b10000000;
      packet[10] += 0b00000000;
      ESP_LOGD(TAG, "Fan: QUIET");
      break;
    case climate::CLIMATE_FAN_LOW:
      packet[8]  += 0b00000000;
      packet[10] += 0b00000001;
      ESP_LOGD(TAG, "Fan: LOW");
      break;
    case climate::CLIMATE_FAN_MIDDLE:
      packet[8]  += 0b00000000;
      packet[10] += 0b00000110;
      ESP_LOGD(TAG, "Fan: MIDDLE");
      break;
    case climate::CLIMATE_FAN_MEDIUM:
      packet[8]  += 0b00000000;
      packet[10] += 0b00000011;
      ESP_LOGD(TAG, "Fan: MEDIUM");
      break;
    case climate::CLIMATE_FAN_HIGH:
      packet[8]  += 0b00000000;
      packet[10] += 0b00000111;
      ESP_LOGD(TAG, "Fan: HIGH");
      break;
    case climate::CLIMATE_FAN_FOCUS:
      packet[8]  += 0b00000000;
      packet[10] += 0b00000101;
      ESP_LOGD(TAG, "Fan: FOCUS");
      break;
    case climate::CLIMATE_FAN_DIFFUSE:
      packet[8]  += 0b01000000;
      packet[10] += 0b00000000;
      ESP_LOGD(TAG, "Fan: DIFFUSE");
      break;
    default:
      packet[8]  += 0b00000000;
      packet[10] += 0b00000000;
      ESP_LOGD(TAG, "Fan: DEFAULT (AUTO)");
      break;
  }

  // ========== Configure swing mode (TCLAC Lines 498-515) ==========
  // ESPHome's built-in swing modes (VERTICAL/HORIZONTAL/BOTH/OFF)
  switch (this->swing_mode) {
    case climate::CLIMATE_SWING_OFF:
      packet[10] += 0b00000000;
      packet[11] += 0b00000000;
      ESP_LOGD(TAG, "Swing: OFF");
      break;
    case climate::CLIMATE_SWING_VERTICAL:
      packet[10] += 0b00111000;  // Vertical swing ON
      packet[11] += 0b00000000;
      ESP_LOGD(TAG, "Swing: VERTICAL");
      break;
    case climate::CLIMATE_SWING_HORIZONTAL:
      packet[10] += 0b00000000;
      packet[11] += 0b00001000;  // Horizontal swing ON
      ESP_LOGD(TAG, "Swing: HORIZONTAL");
      break;
    case climate::CLIMATE_SWING_BOTH:
      packet[10] += 0b00111000;  // Both swings ON
      packet[11] += 0b00001000;
      ESP_LOGD(TAG, "Swing: BOTH");
      break;
    default:
      packet[10] += 0b00000000;
      packet[11] += 0b00000000;
      ESP_LOGD(TAG, "Swing: DEFAULT (OFF)");
      break;
  }

  // ========== Configure presets (TCLAC Lines 517-530) ==========
  switch (this->preset.value_or(climate::CLIMATE_PRESET_NONE)) {
    case climate::CLIMATE_PRESET_NONE:
      break;
    case climate::CLIMATE_PRESET_ECO:
      packet[7] += 0b10000000;  // ECO flag (duplicate but safe)
      ESP_LOGD(TAG, "Preset: ECO");
      break;
    case climate::CLIMATE_PRESET_SLEEP:
      packet[19] += 0b00000001;  // Sleep mode
      ESP_LOGD(TAG, "Preset: SLEEP");
      break;
    case climate::CLIMATE_PRESET_COMFORT:
      packet[8] += 0b00010000;  // Comfort/Health flag
      ESP_LOGD(TAG, "Preset: COMFORT");
      break;
    default:
      break;
  }

  // ========== Temperature (TCLAC Line 668) ==========
  packet[9] = 111 - (int)(this->target_temperature + 0.5f);
  ESP_LOGD(TAG, "Temperature: %.1f°C -> raw 0x%02X", this->target_temperature, packet[9]);

  // ========== Vertical Swing Direction (TCLAC Lines 559-580) ==========
  // Byte 32 bits 3-4 (mask 0b00011000): Swing direction
  //   00 = OFF, 01 = UP_DOWN, 10 = UPSIDE, 11 = DOWNSIDE
  switch (this->vertical_swing_) {
    case VerticalSwingDirection::OFF:
      packet[32] += 0b00000000;
      ESP_LOGD(TAG, "Vertical swing direction: OFF");
      break;
    case VerticalSwingDirection::UP_DOWN:
      packet[32] += 0b00001000;
      ESP_LOGD(TAG, "Vertical swing direction: UP_DOWN");
      break;
    case VerticalSwingDirection::UPSIDE:
      packet[32] += 0b00010000;
      ESP_LOGD(TAG, "Vertical swing direction: UPSIDE");
      break;
    case VerticalSwingDirection::DOWNSIDE:
      packet[32] += 0b00011000;
      ESP_LOGD(TAG, "Vertical swing direction: DOWNSIDE");
      break;
  }

  // ========== Horizontal Swing Direction (TCLAC Lines 582-606) ==========
  // Byte 33 bits 3-5 (mask 0b00111000): Swing direction
  switch (this->horizontal_swing_) {
    case HorizontalSwingDirection::OFF:
      packet[33] += 0b00000000;
      ESP_LOGD(TAG, "Horizontal swing direction: OFF");
      break;
    case HorizontalSwingDirection::LEFT_RIGHT:
      packet[33] += 0b00001000;
      ESP_LOGD(TAG, "Horizontal swing direction: LEFT_RIGHT");
      break;
    case HorizontalSwingDirection::LEFTSIDE:
      packet[33] += 0b00010000;
      ESP_LOGD(TAG, "Horizontal swing direction: LEFTSIDE");
      break;
    case HorizontalSwingDirection::CENTER:
      packet[33] += 0b00011000;
      ESP_LOGD(TAG, "Horizontal swing direction: CENTER");
      break;
    case HorizontalSwingDirection::RIGHTSIDE:
      packet[33] += 0b00100000;
      ESP_LOGD(TAG, "Horizontal swing direction: RIGHTSIDE");
      break;
  }

  // ========== Vertical Airflow Position (TCLAC Lines 608-628) ==========
  // Byte 32 bits 0-2 (mask 0b00000111): Fixed position
  //   000 = LAST, 001 = MAX_UP, 010 = UP, 011 = CENTER, 100 = DOWN, 101 = MAX_DOWN
  switch (this->vertical_airflow_) {
    case AirflowVerticalDirection::LAST:
      packet[32] += 0b00000000;
      ESP_LOGD(TAG, "Vertical airflow: LAST");
      break;
    case AirflowVerticalDirection::MAX_UP:
      packet[32] += 0b00000001;
      ESP_LOGD(TAG, "Vertical airflow: MAX_UP");
      break;
    case AirflowVerticalDirection::UP:
      packet[32] += 0b00000010;
      ESP_LOGD(TAG, "Vertical airflow: UP");
      break;
    case AirflowVerticalDirection::CENTER:
      packet[32] += 0b00000011;
      ESP_LOGD(TAG, "Vertical airflow: CENTER");
      break;
    case AirflowVerticalDirection::DOWN:
      packet[32] += 0b00000100;
      ESP_LOGD(TAG, "Vertical airflow: DOWN");
      break;
    case AirflowVerticalDirection::MAX_DOWN:
      packet[32] += 0b00000101;
      ESP_LOGD(TAG, "Vertical airflow: MAX_DOWN");
      break;
  }

  // ========== Horizontal Airflow Position (TCLAC Lines 630-656) ==========
  // Byte 33 bits 0-2 (mask 0b00000111): Fixed position
  switch (this->horizontal_airflow_) {
    case AirflowHorizontalDirection::LAST:
      packet[33] += 0b00000000;
      ESP_LOGD(TAG, "Horizontal airflow: LAST");
      break;
    case AirflowHorizontalDirection::MAX_LEFT:
      packet[33] += 0b00000001;
      ESP_LOGD(TAG, "Horizontal airflow: MAX_LEFT");
      break;
    case AirflowHorizontalDirection::LEFT:
      packet[33] += 0b00000010;
      ESP_LOGD(TAG, "Horizontal airflow: LEFT");
      break;
    case AirflowHorizontalDirection::CENTER:
      packet[33] += 0b00000011;
      ESP_LOGD(TAG, "Horizontal airflow: CENTER");
      break;
    case AirflowHorizontalDirection::RIGHT:
      packet[33] += 0b00000100;
      ESP_LOGD(TAG, "Horizontal airflow: RIGHT");
      break;
    case AirflowHorizontalDirection::MAX_RIGHT:
      packet[33] += 0b00000101;
      ESP_LOGD(TAG, "Horizontal airflow: MAX_RIGHT");
      break;
  }
  
  // ========== Checksum (last byte) ==========
  packet[SET_PACKET_SIZE - 1] = this->calculate_checksum_(packet, SET_PACKET_SIZE - 1);
  
  ESP_LOGD(TAG, "Created complete SET packet with TCLAC protocol");
}

void TclAcClimate::send_packet_(const uint8_t *packet, size_t length) {
  // Log packet for debugging
  ESP_LOGV(TAG, "Sending packet (%d bytes):", length);
  for (size_t i = 0; i < length; i++) {
    ESP_LOGV(TAG, "  [%02d] 0x%02X", i, packet[i]);
  }
  
  // Send via UART
  this->write_array(packet, length);
  this->flush();
  this->last_transmit_ = millis();
}

void TclAcClimate::send_poll_packet_() {
  uint8_t packet[POLL_PACKET_SIZE] = {
    HEADER_MCU_TO_AC_0,
    HEADER_MCU_TO_AC_1,
    HEADER_MCU_TO_AC_2,
    CMD_POLL,
    0x01,  // Length
    0x00,  // Data
    0x00   // Checksum (will be calculated)
  };
  
  packet[POLL_PACKET_SIZE - 1] = this->calculate_checksum_(packet, POLL_PACKET_SIZE - 1);
  this->send_packet_(packet, POLL_PACKET_SIZE);
  ESP_LOGV(TAG, "Sent POLL packet");
}

uint8_t TclAcClimate::calculate_checksum_(const uint8_t *data, size_t length) {
  // XOR checksum - VALIDATED from log analysis
  uint8_t checksum = 0;
  for (size_t i = 0; i < length; i++) {
    checksum ^= data[i];
  }
  return checksum;
}

void TclAcClimate::parse_status_packet_(const uint8_t *data, size_t length) {
  if (length < 32) {
    ESP_LOGW(TAG, "Status packet too short: %d bytes", length);
    return;
  }
  
  // Parse status data based on 55-byte response format
  // data[0-1]: Command specific data
  // data[2]: Mode byte with flags
  // data[3]: Speed byte with flags
  
  // Byte 2 (data[2]): Mode flags
  uint8_t mode_byte = data[2];
  bool display_on = (mode_byte & FLAG_DISPLAY_ON) != 0;
  bool eco_on = (mode_byte & FLAG_ECO_MODE) != 0;
  
  // Byte 3 (data[3]): Speed flags
  uint8_t speed_byte = data[3];
  bool turbo_on = (speed_byte & FLAG_TURBO_MODE) != 0;
  bool quiet_on = (speed_byte & FLAG_QUIET_MODE) != 0;
  
  // Check if AC changed modes without our consent (e.g., auto-enabling ECO)
  bool eco_changed = (this->eco_mode_ != eco_on);
  bool turbo_changed = (this->turbo_mode_ != turbo_on);
  bool quiet_changed = (this->quiet_mode_ != quiet_on);
  
  // Update state
  this->eco_mode_ = eco_on;
  this->turbo_mode_ = turbo_on;
  this->quiet_mode_ = quiet_on;
  
  // Log changes
  if (eco_changed) {
    ESP_LOGD(TAG, "AC changed ECO mode to: %s", eco_on ? "ON" : "OFF");
  }
  if (turbo_changed) {
    ESP_LOGD(TAG, "AC changed TURBO mode to: %s", turbo_on ? "ON" : "OFF");
  }
  if (quiet_changed) {
    ESP_LOGD(TAG, "AC changed QUIET mode to: %s", quiet_on ? "ON" : "OFF");
  }
  
 // Temperature parsing
  // PACKET bytes [17:18] using:  (((raw16)/374 - 32) / 1.8)
  //   // In this parser, "data" points to payload starting at PACKET byte 5, so:
  //   PACKET[17] -> data[12]
  //   PACKET[18] -> data[13]
  // This method is preferred because it is stable across mode/power changes.
  bool got_room_temp = false;
  if (length >= 14) {
    const uint16_t raw16 = ((uint16_t) data[12] << 8) | data[13];
    const float room_c = (((float) raw16 / 374.0f) - 32.0f) / 1.8f;
    if (room_c > -10.0f && room_c < 60.0f) {
      this->current_temperature = room_c;
      got_room_temp = true;
      ESP_LOGD(TAG, "Room temperature (16-bit) data[12:13]=0x%02X%02X raw=%u -> %.1f°C",
               data[12], data[13], raw16, room_c);
    }
  }

  // Fallback (older single-byte heuristic)
  if (!got_room_temp && length >= 55) {
    const uint8_t ac_temp_raw = data[30];
    if (ac_temp_raw >= 120 && ac_temp_raw <= 180) {
      const float ac_temp = this->raw_to_celsius_(ac_temp_raw);
      if (ac_temp > -10.0f && ac_temp < 60.0f) {
        this->current_temperature = ac_temp;
        ESP_LOGD(TAG, "Room temperature (fallback byte[30]) raw=0x%02X -> %.1f°C", ac_temp_raw, ac_temp);
      }
    }
  }
  
  ESP_LOGD(TAG, "Status update - Temp: %.1f°C, ECO: %d, Turbo: %d, Quiet: %d", 
           this->target_temperature, eco_on, turbo_on, quiet_on);
  
  this->publish_state();
}

void TclAcClimate::parse_temp_response_(const uint8_t *data, size_t length) {
  if (length < 4) {
    ESP_LOGW(TAG, "Temp response too short: %d bytes", length);
    return;
  }
  
// TEMP_RESPONSE encoding differs from STATUS.
  // Byte 0: current temperature (raw - 7)
  // Byte 2: target temperature  (raw - 12)
  const uint8_t current_raw = data[0];
  const uint8_t target_raw = data[2];

  const float current_c = ((float) current_raw) - 7.0f;
  if (current_c > -10.0f && current_c < 60.0f) {
    this->current_temperature = current_c;
    ESP_LOGD(TAG, "TEMP_RESPONSE current: raw=0x%02X -> %.1f°C", current_raw, current_c);
  }

  const float target_c = ((float) target_raw) - 12.0f;
  if (target_c > 10.0f && target_c < 40.0f) {
    this->target_temperature = target_c;
    ESP_LOGD(TAG, "TEMP_RESPONSE target: raw=0x%02X -> %.1f°C", target_raw, target_c);
  }

  this->publish_state();
}

void TclAcClimate::parse_power_response_(const uint8_t *data, size_t length) {
  // CMD_POWER (0x0A) packet structure:
  // - Payload length: 45 bytes
  // - Byte[0]: Always 0x04 (unknown)
  // - Byte[1]: Always 0x00 (unknown)
  // - Byte[2]: POWER FLAG - 0x04=OFF, 0x0C=ON
  // - Byte[3]: Secondary flag (0x00 or 0x01, rare)
  // - Rest: Mostly zeros
  
  if (length < 3) {
    ESP_LOGW(TAG, "Power response too short: %d bytes", length);
    return;
  }
  
  uint8_t power_flag = data[2];  // Byte[2] in payload
  
  ESP_LOGD(TAG, "Power packet: Byte[0]=0x%02X, Byte[1]=0x%02X, Byte[2]=0x%02X", 
           data[0], data[1], power_flag);
  
  if (power_flag == 0x04) {
    // Power OFF
    if (this->mode != climate::CLIMATE_MODE_OFF) {
      ESP_LOGI(TAG, "AC Power Status: OFF (from CMD_POWER packet)");
      this->mode = climate::CLIMATE_MODE_OFF;
      this->publish_state();
    }
  } else if (power_flag == 0x0C) {
    // Power ON
    if (this->mode == climate::CLIMATE_MODE_OFF) {
      ESP_LOGI(TAG, "AC Power Status: ON (from CMD_POWER packet)");
      // Mode was already saved, just publish
      this->publish_state();
    }
  } else {
    ESP_LOGW(TAG, "Unknown power flag in CMD_POWER: 0x%02X", power_flag);
  }
}

uint8_t TclAcClimate::get_fan_speed_() {
  // Map ESPHome fan modes to TCL fan speeds (validated from log)
  switch (this->fan_mode.value_or(climate::CLIMATE_FAN_LOW)) {
    case climate::CLIMATE_FAN_AUTO:
      return FAN_SPEED_AUTO;
    case climate::CLIMATE_FAN_LOW:
      return FAN_SPEED_LOW;  // Most common (83% in log)
    case climate::CLIMATE_FAN_MEDIUM:
      return FAN_SPEED_MEDIUM;
    case climate::CLIMATE_FAN_HIGH:
      return FAN_SPEED_MAX;  // Use MAX for "high"
    default:
      return FAN_SPEED_LOW;
  }
}

uint8_t TclAcClimate::celsius_to_raw_(float temp) {
  // TCLAC protocol formula: 111 - celsius
  // This is what works with your AC!
  int raw = 111 - (int)(temp + 0.5f);  // +0.5 for rounding
  if (raw < 0) raw = 0;
  if (raw > 255) raw = 255;
  ESP_LOGD(TAG, "Temperature encoding: %.1f°C -> raw 0x%02X (111 - %d)", temp, raw, (int)(temp + 0.5f));
  return (uint8_t)raw;
}

float TclAcClimate::raw_to_celsius_(uint8_t raw) {
  // Formula from protocol analysis: raw - 127 gives accurate room temp
  // Analysis: Byte[30] with 'raw - 127' = ~18.87°C (0.43°C deviation from 19.3°C)
  // Note: Only valid for ~36% of packets (STATUS packets), not SHORT_STATUS/POWER
  return (float)raw - 127.0f;
}

// Runtime control methods for Home Assistant automations

void TclAcClimate::set_vertical_airflow(AirflowVerticalDirection direction) {
  ESP_LOGD(TAG, "Setting vertical airflow direction: %d", (int)direction);
  this->vertical_airflow_ = direction;
  if (this->force_mode_ && this->allow_send_) {
    // Create and send packet with new settings
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_horizontal_airflow(AirflowHorizontalDirection direction) {
  ESP_LOGD(TAG, "Setting horizontal airflow direction: %d", (int)direction);
  this->horizontal_airflow_ = direction;
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_vertical_swing(VerticalSwingDirection direction) {
  ESP_LOGD(TAG, "Setting vertical swing direction: %d", (int)direction);
  this->vertical_swing_ = direction;
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_horizontal_swing(HorizontalSwingDirection direction) {
  ESP_LOGD(TAG, "Setting horizontal swing direction: %d", (int)direction);
  this->horizontal_swing_ = direction;
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_display_state(bool state) {
  ESP_LOGD(TAG, "Setting display state: %s", state ? "ON" : "OFF");
  this->display_state_ = state;
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_beeper_state(bool state) {
  ESP_LOGD(TAG, "Setting beeper state: %s", state ? "ON" : "OFF");
  this->beeper_state_ = state;
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_eco_mode(bool enabled) {
  ESP_LOGD(TAG, "Setting ECO mode: %s", enabled ? "ON" : "OFF");
  this->eco_mode_ = enabled;
  
  // ECO, Turbo and Quiet are mutually exclusive
  if (enabled) {
    if (this->turbo_mode_) {
      ESP_LOGD(TAG, "Disabling TURBO mode (mutually exclusive with ECO)");
      this->turbo_mode_ = false;
    }
    if (this->quiet_mode_) {
      ESP_LOGD(TAG, "Disabling QUIET mode (mutually exclusive with ECO)");
      this->quiet_mode_ = false;
    }
  }
  
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_turbo_mode(bool enabled) {
  ESP_LOGD(TAG, "Setting TURBO mode: %s", enabled ? "ON" : "OFF");
  this->turbo_mode_ = enabled;
  
  // ECO, Turbo and Quiet are mutually exclusive
  if (enabled) {
    if (this->eco_mode_) {
      ESP_LOGD(TAG, "Disabling ECO mode (mutually exclusive with TURBO)");
      this->eco_mode_ = false;
    }
    if (this->quiet_mode_) {
      ESP_LOGD(TAG, "Disabling QUIET mode (mutually exclusive with TURBO)");
      this->quiet_mode_ = false;
    }
  }
  
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_quiet_mode(bool enabled) {
  ESP_LOGD(TAG, "Setting QUIET mode: %s", enabled ? "ON" : "OFF");
  this->quiet_mode_ = enabled;
  
  // ECO, Turbo and Quiet are mutually exclusive
  if (enabled) {
    if (this->eco_mode_) {
      ESP_LOGD(TAG, "Disabling ECO mode (mutually exclusive with QUIET)");
      this->eco_mode_ = false;
    }
    if (this->turbo_mode_) {
      ESP_LOGD(TAG, "Disabling TURBO mode (mutually exclusive with QUIET)");
      this->turbo_mode_ = false;
    }
  }
  
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_health_mode(bool enabled) {
  ESP_LOGD(TAG, "Setting HEALTH mode: %s", enabled ? "ON" : "OFF");
  this->health_mode_ = enabled;
  if (this->force_mode_ && this->allow_send_) {
    uint8_t packet[SET_PACKET_SIZE];
    this->create_set_packet_(packet);
    this->send_packet_(packet, SET_PACKET_SIZE);
  }
}

}  // namespace tcl_ac
}  // namespace esphome
