// Thin Arduino entry points.
//
// The real orchestration lives in `runtime.cpp`; `main.cpp` simply maps the
// Arduino `setup()` and `loop()` hooks onto that module.

#include "runtime.h"

// Arduino boot hook. Delegates to the runtime orchestrator.
void setup() {
  setupApp();
}

// Arduino main loop hook. Delegates to the runtime orchestrator.
void loop() {
  loopApp();
}
