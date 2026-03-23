// Serial command handling for manual diagnostics and configuration.
//
// This interface keeps the runtime module from owning command parsing details.

#pragma once

// Holds the board at startup so a user can enter commands before automation
// begins. Only active on non-timer boots.
void handleSerialConfigWindow();

// Drains the serial input buffer and dispatches any complete commands.
void pollSerialCommands();
