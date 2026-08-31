// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AutoControllerAssignment.h"

#include <algorithm>
#include <string>
#include <vector>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "Core/System.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/InputConfig.h"

AutoControllerAssignment::AutoControllerAssignment() = default;

AutoControllerAssignment::~AutoControllerAssignment()
{
  Shutdown();
}

void AutoControllerAssignment::Initialize()
{
  if (!Config::Get(Config::MAIN_AUTO_CONTROLLER_ASSIGNMENT))
    return;

  // Register for device change notifications.
  m_devices_changed_hook = g_controller_interface.RegisterDevicesChangedCallback([this] {
    if (Config::Get(Config::MAIN_AUTO_CONTROLLER_ASSIGNMENT))
      PerformAssignment();
  });

  // Perform initial assignment with currently connected devices.
  PerformAssignment();
}

void AutoControllerAssignment::Shutdown()
{
  m_devices_changed_hook.reset();
}

void AutoControllerAssignment::PerformAssignment()
{
  const auto sdl_devices = GetConnectedSDLDevices();

  INFO_LOG_FMT(CONTROLLERINTERFACE,
               "AutoControllerAssignment: {} SDL device(s) connected, performing assignment",
               sdl_devices.size());

  AssignGCPadSlots(sdl_devices);

  // If the system is a Wii, also assign to Wiimote emulation slots.
  if (Core::System::GetInstance().IsWii())
    AssignWiimoteSlots(sdl_devices);
}

bool AutoControllerAssignment::IsNintendoController(const std::string& device_name)
{
  // Known Nintendo controller name patterns that indicate a Wiimote or Pro Controller
  // connected via Bluetooth (these would typically be handled via BT passthrough).
  static const std::vector<std::string> nintendo_patterns = {
      "Nintendo RVL-CNT",  // Wiimote
      "Nintendo RVL-WBC",  // Balance Board
      "Wii Remote",        // Generic Wiimote name in some SDL versions
      "Nintendo Wii",      // Wii controller variants
  };

  for (const auto& pattern : nintendo_patterns)
  {
    if (device_name.find(pattern) != std::string::npos)
      return true;
  }

  return false;
}

std::vector<std::string> AutoControllerAssignment::GetConnectedSDLDevices() const
{
  std::vector<std::string> sdl_devices;

  const auto all_devices = g_controller_interface.GetAllDevices();
  for (const auto& device : all_devices)
  {
    if (device->GetSource() == "SDL")
    {
      sdl_devices.push_back(device->GetQualifiedName());
    }
  }

  return sdl_devices;
}

void AutoControllerAssignment::AssignGCPadSlots(const std::vector<std::string>& sdl_devices)
{
  InputConfig* gc_config = Pad::GetConfig();
  if (!gc_config)
    return;

  const int num_slots = gc_config->GetControllerCount();
  const int num_devices = static_cast<int>(sdl_devices.size());

  for (int slot = 0; slot < num_slots; ++slot)
  {
    auto* controller = gc_config->GetController(slot);
    if (!controller)
      continue;

    if (slot < num_devices)
    {
      const auto& device_string = sdl_devices[slot];

      // Skip Nintendo controllers for GCPad slots — they're better handled
      // as Wiimotes via BT passthrough or Real source.
      // However, Pro Controllers work fine as GCPad, so only skip actual Wiimotes.
      const auto current_device = controller->GetDefaultDevice().ToString();
      if (current_device != device_string)
      {
        INFO_LOG_FMT(CONTROLLERINTERFACE,
                     "AutoControllerAssignment: Assigning GCPad slot {} to '{}'", slot + 1,
                     device_string);
        controller->SetDefaultDevice(device_string);
        controller->UpdateReferences(g_controller_interface);
      }
    }
    else
    {
      // No device available for this slot — clear it so the slot is inactive.
      const auto current_device = controller->GetDefaultDevice().ToString();
      if (!current_device.empty())
      {
        INFO_LOG_FMT(CONTROLLERINTERFACE,
                     "AutoControllerAssignment: Clearing GCPad slot {} (no device available)",
                     slot + 1);
        controller->SetDefaultDevice("");
        controller->UpdateReferences(g_controller_interface);
      }
    }
  }
}

void AutoControllerAssignment::AssignWiimoteSlots(const std::vector<std::string>& sdl_devices)
{
  InputConfig* wii_config = Wiimote::GetConfig();
  if (!wii_config)
    return;

  // If Bluetooth passthrough is enabled, we don't manage Wiimote slots at all —
  // the real BT adapter handles everything.
  if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    return;

  const int num_slots = std::min(wii_config->GetControllerCount(), static_cast<int>(MAX_WIIMOTES));
  const int num_devices = static_cast<int>(sdl_devices.size());

  for (int slot = 0; slot < num_slots; ++slot)
  {
    auto* controller = wii_config->GetController(slot);
    if (!controller)
      continue;

    if (slot < num_devices)
    {
      const auto& device_string = sdl_devices[slot];

      // Extract device name from qualified name "SDL/{id}/{name}"
      const auto last_slash = device_string.rfind('/');
      const std::string device_name =
          (last_slash != std::string::npos) ? device_string.substr(last_slash + 1) : device_string;

      if (IsNintendoController(device_name))
      {
        // Nintendo controller detected — set source to Real so Dolphin's
        // Wiimote scanning can pick it up via Bluetooth.
        INFO_LOG_FMT(CONTROLLERINTERFACE,
                     "AutoControllerAssignment: Nintendo controller '{}' detected for Wiimote "
                     "slot {}, setting source to Real",
                     device_name, slot + 1);
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(slot), WiimoteSource::Real);
      }
      else
      {
        // Regular SDL controller — assign to emulated Wiimote slot.
        // This enables gyro-capable controllers to use SDL's motion sensors.
        const auto current_device = controller->GetDefaultDevice().ToString();
        if (current_device != device_string)
        {
          INFO_LOG_FMT(CONTROLLERINTERFACE,
                       "AutoControllerAssignment: Assigning Wiimote slot {} to '{}'", slot + 1,
                       device_string);
          Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(slot), WiimoteSource::Emulated);
          controller->SetDefaultDevice(device_string);
          controller->UpdateReferences(g_controller_interface);
        }
      }
    }
    else
    {
      // No device for this slot — set to None.
      const auto current_source = Config::Get(Config::GetInfoForWiimoteSource(slot));
      if (current_source != WiimoteSource::None)
      {
        INFO_LOG_FMT(CONTROLLERINTERFACE,
                     "AutoControllerAssignment: Clearing Wiimote slot {} (no device available)",
                     slot + 1);
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(slot), WiimoteSource::None);
        controller->SetDefaultDevice("");
        controller->UpdateReferences(g_controller_interface);
      }
    }
  }
}
