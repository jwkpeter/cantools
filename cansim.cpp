/* CAN bus to UDP simulation for development purposes
 */


#include <linux/can.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <iostream>

#include "cxxopts.hpp"

#include "timer.h"
#include "udpsocket.h"
#include "priority.h"


// Evil functions to defeat the optimizer
__attribute__((unused)) static void escape(void* p) { asm volatile("" : : "g"(p) : "memory"); }
__attribute__((unused)) static void clobber() { asm volatile("" : : : "memory"); }


namespace
{


#pragma pack(push, 1)

struct Frame_veh_state
{
  std::uint8_t crc;
  std::uint8_t aliv : 4;
  std::uint8_t velocity_0 : 4;
  std::uint8_t velocity_1;
  std::uint8_t velocity_2 : 2;
  std::uint8_t wiper_position_0 : 6;
  std::uint8_t wiper_position_1;
  std::uint8_t wiper_position_2 : 2;
};

struct Frame_flux
{
  std::uint32_t power_level;
  std::uint32_t dispersal_rate;
};

struct Frame_date_time
{
  std::uint8_t time_type : 2;
  std::uint8_t year_0 : 6;
  std::uint8_t year_1;
  std::uint8_t month : 4;
  std::uint8_t day_0 : 4;
  std::uint8_t day_1 : 1;
  std::uint8_t am_pm : 1;
  std::uint8_t hour : 4;
  std::uint8_t minute_0 : 2;
  std::uint8_t minute_1 : 4;
};


struct Frame_fuel
{
  std::uint8_t fuel_type : 1;
  std::uint8_t reserved : 7;
  union {
    struct {
      std::uint8_t e_range;
      std::uint8_t e_cons_0;
      std::uint8_t e_cons_1 : 2;
      std::uint8_t bat_volt_0 : 6;
      std::uint8_t bat_volt_1 : 6;
      std::uint8_t charging : 1;
      std::uint8_t super_charging : 1;
    } m0;
    struct {
      std::uint8_t gas_range_0;
      std::uint8_t gas_range_1 : 2;
      std::uint8_t gas_cons_0 : 6;
      std::uint8_t gas_cons_1 : 4;
    } m1;
  } mux;
};

#pragma pack(pop)


}  // namespace


void simulate(std::atomic<bool>& stop, std::string&& ip, std::uint16_t port, bool timestamp)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"

  // Example frames
  can_frame veh_state{0};
  veh_state.can_id = 201;
  veh_state.can_dlc = 8;
  auto* veh_state_data = reinterpret_cast<Frame_veh_state*>(veh_state.data);
  veh_state_data->crc = 0xFF;
  veh_state_data->aliv = 0;
  veh_state_data->velocity_0 = 8800;
  veh_state_data->velocity_1 = 8800 >> 4;
  veh_state_data->velocity_2 = 8800 >> 12;
  veh_state_data->wiper_position_0 = 2122;
  veh_state_data->wiper_position_1 = 2122 >> 4;
  veh_state_data->wiper_position_2 = 2122 >> 12;

  can_frame flux{0};
  flux.can_id = 233;
  flux.can_dlc = 6;
  auto* flux_data = reinterpret_cast<Frame_flux*>(flux.data);
  flux_data->power_level = 1210000000;
  flux_data->dispersal_rate = 7743;

  can_frame date_time{0};
  date_time.can_id = 245;
  date_time.can_dlc = 5;
  auto* date_time_data = reinterpret_cast<Frame_date_time*>(date_time.data);
  date_time_data->year_0 = 1955;
  date_time_data->year_1 = 1955 >> 6;
  date_time_data->month = 11;
  date_time_data->day_0 = 5;
  date_time_data->day_1 = 5 >> 4;
  date_time_data->hour = 6;
  date_time_data->minute_0 = 31;
  date_time_data->minute_1 = 31 >> 2;

  can_frame fuel{0};
  fuel.can_id = 502;
  fuel.can_dlc = 5;
  Frame_fuel electric;
  electric.mux.m0.e_range = 149;
  electric.mux.m0.e_cons_0 = 354;
  electric.mux.m0.e_cons_1 = 354 >> 8;
  electric.mux.m0.bat_volt_0 = 3751;
  electric.mux.m0.bat_volt_1 = 3751 >> 6;
  electric.mux.m0.charging = 0;
  electric.mux.m0.super_charging = 0;
  Frame_fuel gasoline;
  gasoline.mux.m1.gas_range_0 = 381;
  gasoline.mux.m1.gas_range_1 = 381 >> 8;
  gasoline.mux.m1.gas_cons_0 = 178;
  gasoline.mux.m1.gas_cons_1 = 178 >> 6;
  auto* fuel_data = reinterpret_cast<Frame_fuel*>(fuel.data);
  fuel_data->reserved = 0;
  fuel_data->fuel_type = 0;
  fuel_data->mux = electric.mux;

#pragma GCC diagnostic pop

  can_frame radar{0};
  radar.can_id = 402;
  radar.can_dlc = 4;
  // Motorola byte order
  // Pos 7, len 14 = 12317
  // Pos 9, len 14 = 4404
  // Pos 27, len 4 = 11
  radar.data[0] = 0b1100'0000;
  radar.data[1] = 0b0111'0101;
  radar.data[2] = 0b0001'0011;
  radar.data[3] = 0b0100'1011;

  // Network inferface and timer
  std::vector<std::uint8_t> buffer(sizeof(std::uint64_t) + sizeof(can_frame));
  auto* time_buffer = reinterpret_cast<std::uint64_t*>(buffer.data());
  auto* frame_buffer = reinterpret_cast<std::uint64_t*>(buffer.data() + sizeof(std::uint64_t));
  udp::Socket udp_socket;
  util::Timer timer;
  try {
    udp_socket.open(ip, port);
    timer.init_system_timer();
  }
  catch (const std::runtime_error& e) {
    std::cerr << e.what() << std::endl;
    return;
  }

  // Transmit scheduling
  auto transmit = [&](std::uint64_t time_ms) {
    if (time_ms % 250 == 0) {  // 250 ms cycle time
      veh_state_data->aliv++;
      // Add crc calculation
      udp_socket.transmit(&veh_state);
    }
    if (time_ms % 100 == 0) {
      udp_socket.transmit(&flux);
    }
    if (time_ms % 225 == 0) {
      udp_socket.transmit(&radar);
    }
    if (time_ms % 1333 == 0) {
      udp_socket.transmit(&date_time);
    }
    if (time_ms % 500 == 0) {
      fuel_data->fuel_type = ~fuel_data->fuel_type;
      if (fuel_data->fuel_type == 0)
        fuel_data->mux = electric.mux;
      else
        fuel_data->mux = gasoline.mux;
      udp_socket.transmit(&fuel);
    }
  };

  auto transmit_with_timestamp = [&](std::uint64_t time_ms) {
    if (time_ms % 250 == 0) {  // 250 ms cycle time
      veh_state_data->aliv++;
      // Add crc calculation
      *time_buffer = time_ms;
      std::memcpy(frame_buffer, &veh_state, sizeof(can_frame));
      udp_socket.transmit(buffer);
    }
    if (time_ms % 100 == 0) {
      *time_buffer = time_ms;
      std::memcpy(frame_buffer, &flux, sizeof(can_frame));
      udp_socket.transmit(buffer);
    }
    if (time_ms % 225 == 0) {
      *time_buffer = time_ms;
      std::memcpy(frame_buffer, &radar, sizeof(can_frame));
      udp_socket.transmit(buffer);
    }
    if (time_ms % 1333 == 0) {
      *time_buffer = time_ms;
      std::memcpy(frame_buffer, &date_time, sizeof(can_frame));
      udp_socket.transmit(buffer);
    }
    if (time_ms % 500 == 0) {
      fuel_data->fuel_type = ~fuel_data->fuel_type;
      if (fuel_data->fuel_type == 0) {
        // Update range
        if (electric.mux.m0.super_charging == 1) electric.mux.m0.e_range += 4;
        else if (electric.mux.m0.charging == 1) electric.mux.m0.e_range++;
        else electric.mux.m0.e_range--;
        // Set charging state
        if (electric.mux.m0.e_range == 0) electric.mux.m0.charging = 1;
        if (electric.mux.m0.charging == 1 && electric.mux.m0.e_range == 100) electric.mux.m0.super_charging = 1;
        else if (electric.mux.m0.e_range > 200) electric.mux.m0.charging = electric.mux.m0.super_charging = 0;
        fuel_data->mux = electric.mux;
      }
      else {
        fuel_data->mux = gasoline.mux;
      }

      *time_buffer = time_ms;
      std::memcpy(frame_buffer, &fuel, sizeof(can_frame));
      udp_socket.transmit(buffer);
    }
  };

  // Looping and timing
  auto current_time_us = timer.system_time();
  escape(&current_time_us);  // Prevent infinite loop due to optimizer fixing this value
  auto next_cycle = current_time_us + 1000ull;

  while (!stop.load())
  {
    do {
      current_time_us = timer.system_time();
    } while (current_time_us < next_cycle);  // Polling wait until exactly 1 ms passed
    next_cycle = current_time_us + 1000ull;
    if (timestamp)
      transmit_with_timestamp(current_time_us / 1000ull);
    else
      transmit(current_time_us / 1000ull);
    usleep(750);
  }
}


std::tuple<std::string, std::uint16_t, bool, bool> parse_args(int argc, char** argv)
{
  bool realtime = false;
  bool timestamp = false;
  std::string remote_ip;
  std::uint16_t data_port;

  try {
    cxxopts::Options options{"cansim", "CAN bus to UDP simulation"};
    options.add_options()
      ("r,realtime", "Enable realtime scheduling policy", cxxopts::value<bool>(realtime))
      ("t,timestamp", "Prefix UDP packets with timestamp", cxxopts::value<bool>(timestamp))
      ("i,ip", "Remote device IP", cxxopts::value<std::string>(remote_ip))
      ("p,port", "UDP data port", cxxopts::value<std::uint16_t>(data_port))
    ;
    options.parse(argc, argv);

    if (options.count("ip") == 0) {
      throw std::runtime_error{"Remote IP must be specified, use the -i or --ip option"};
    }
    if (options.count("port") == 0) {
      throw std::runtime_error{"UDP port must be specified, use the -p or --port option"};
    }

    return std::make_tuple(std::move(remote_ip), data_port, realtime, timestamp);
  }
  catch (const cxxopts::OptionException& e) {
    throw std::runtime_error{e.what()};
  }
}


int main(int argc, char** argv)
{
  bool realtime;
  bool timestamp;
  std::string remote_ip;
  std::uint16_t data_port;

  try {
    std::tie(remote_ip, data_port, realtime, timestamp) = parse_args(argc, argv);
  }
  catch (const std::runtime_error& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  std::cout << "Sending frames to " << remote_ip << ":" << data_port
      << "\nPress enter to stop..." << std::endl;

  std::atomic<bool> stop{false};
  std::thread simulation{&simulate, std::ref(stop), std::move(remote_ip), data_port, timestamp};

  if (realtime) {
    if (priority::set_realtime(simulation.native_handle()))
      std::cout << "Simulation thread set to realtime scheduling policy\n";
    else
      std::cout << "Warning: Could not set scheduling policy, forgot sudo?\n";
  }

  std::cin.ignore();  // Wait in main thread

  std::cout << "Stopping simulation..." << std::endl;
  stop.store(true);
  simulation.join();

  std::cout << "Program finished" << std::endl;
  return 0;
}
