#include <cassert>

#include "system/camerad/cameras/camera_common.h"
#include "system/camerad/cameras/camera_qcom2.h"

namespace {

std::map<uint16_t, std::pair<int, int>> ar0231_build_register_lut(CameraState *c, uint8_t *data) {
  // This function builds a lookup table from register address, to a pair of indices in the
  // buffer where to read this address. The buffer contains padding bytes,
  // as well as markers to indicate the type of the next byte.
  //
  // 0xAA is used to indicate the MSB of the address, 0xA5 for the LSB of the address.
  // Every byte of data (MSB and LSB) is preceded by 0x5A. Specifying an address is optional
  // for contiguous ranges. See page 27-29 of the AR0231 Developer guide for more information.

  int max_i[] = {1828 / 2 * 3, 1500 / 2 * 3};
  auto get_next_idx = [](int cur_idx) {
    return (cur_idx % 3 == 1) ? cur_idx + 2 : cur_idx + 1;  // Every third byte is padding
  };

  std::map<uint16_t, std::pair<int, int>> registers;
  for (int register_row = 0; register_row < 2; register_row++) {
    uint8_t *registers_raw = data + c->ci.frame_stride * register_row;
    assert(registers_raw[0] == 0x0a);  // Start of line

    int value_tag_count = 0;
    int first_val_idx = 0;
    uint16_t cur_addr = 0;

    for (int i = 1; i <= max_i[register_row]; i = get_next_idx(get_next_idx(i))) {
      int val_idx = get_next_idx(i);

      uint8_t tag = registers_raw[i];
      uint16_t val = registers_raw[val_idx];

      if (tag == 0xAA) {  // Register MSB tag
        cur_addr = val << 8;
      } else if (tag == 0xA5) {  // Register LSB tag
        cur_addr |= val;
        cur_addr -= 2;           // Next value tag will increment address again
      } else if (tag == 0x5A) {  // Value tag

        // First tag
        if (value_tag_count % 2 == 0) {
          cur_addr += 2;
          first_val_idx = val_idx;
        } else {
          registers[cur_addr] = std::make_pair(first_val_idx + c->ci.frame_stride * register_row, val_idx + c->ci.frame_stride * register_row);
        }

        value_tag_count++;
      }
    }
  }
  return registers;
}

std::map<uint16_t, uint16_t> ar0231_parse_registers(CameraState *c, uint8_t *data, std::initializer_list<uint16_t> addrs) {
  if (c->ar0231_register_lut.empty()) {
    c->ar0231_register_lut = ar0231_build_register_lut(c, data);
  }

  std::map<uint16_t, uint16_t> registers;
  for (uint16_t addr : addrs) {
    auto offset = c->ar0231_register_lut[addr];
    registers[addr] = ((uint16_t)data[offset.first] << 8) | data[offset.second];
  }
  return registers;
}

float ar0231_parse_temp_sensor(uint16_t calib1, uint16_t calib2, uint16_t data_reg) {
  // See AR0231 Developer Guide - page 36
  float slope = (125.0 - 55.0) / ((float)calib1 - (float)calib2);
  float t0 = 55.0 - slope * (float)calib2;
  return t0 + slope * (float)data_reg;
}

}  // namespace

void ar0231_process_registers(MultiCameraState *s, CameraState *c, cereal::FrameData::Builder &framed) {
  const uint8_t expected_preamble[] = {0x0a, 0xaa, 0x55, 0x20, 0xa5, 0x55};
  uint8_t *data = (uint8_t *)c->buf.cur_camera_buf->addr + c->ci.registers_offset;

  if (memcmp(data, expected_preamble, std::size(expected_preamble)) != 0) {
    LOGE("unexpected register data found");
    return;
  }

  auto registers = ar0231_parse_registers(c, data, {0x2000, 0x2002, 0x20b0, 0x20b2, 0x30c6, 0x30c8, 0x30ca, 0x30cc});

  uint32_t frame_id = ((uint32_t)registers[0x2000] << 16) | registers[0x2002];
  framed.setFrameIdSensor(frame_id);

  float temp_0 = ar0231_parse_temp_sensor(registers[0x30c6], registers[0x30c8], registers[0x20b0]);
  float temp_1 = ar0231_parse_temp_sensor(registers[0x30ca], registers[0x30cc], registers[0x20b2]);
  framed.setTemperaturesC({temp_0, temp_1});
}
