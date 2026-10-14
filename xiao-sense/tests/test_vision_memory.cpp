// Run through run_vision_memory.ps1, which compiles the real service with mocks.
#include "vision_service.cpp"

void reset() {
  release_resources();
  assert(mock::allocations.empty());
  state = {};
  result_at = generation = 0;
  loaded_battery = false;
  worker = nullptr;
  mock::now = 1000;
  mock::allocationCalls = mock::failAllocationAt = 0;
  mock::modelCreated = mock::modelDestroyed = mock::modelRuns = 0;
  mock::inferenceBegins = mock::inferenceEnds = mock::gateDepth = 0;
  mock::decodeCalls = mock::framesReturned = mock::notifications = 0;
  mock::listening = mock::visionEnabled = mock::taskCreationFails = false;
  mock::frameAvailable = true;
  mock::decodeFails = false;
  mock::captureHook = mock::gateHook = mock::runHook = mock::allocateHook = nullptr;
  vision_service::begin();
  assert(state.ready && state.batteryReady && worker);
}
void tick() {
  mock::waitsRemaining = 1;
  try { mock::workerTask(nullptr); }
  catch (const mock::StopWorker&) {}
  assert(mock::gateDepth == 0 && mock::inferenceBegins == mock::inferenceEnds);
}
void enable() { assert(vision_service::setEnabled(true)); }
void checkReleased() { assert(!detector && !jpeg_copy && !rgb565 && mock::allocations.empty()); }

int main() {
  reset();
  tick();
  checkReleased();
  assert(mock::allocationCalls == 0 && mock::modelCreated == 0);
  // Repeated initialization must not spawn an additional owner.
  vision_service::begin();
  assert(mock::allocationCalls == 0);

  enable();
  tick();
  assert(mock::allocatedBytes() == 219136 && state.hasResult && state.personDetected);
  assert(mock::modelCreated == 1 && mock::modelRuns == 1);
  assert(vision_service::setEnabled(false));
  // The control caller must not free anything still owned by the worker.
  assert(mock::allocatedBytes() == 219136);
  tick();
  checkReleased();
  assert(mock::modelDestroyed == 1 && !state.hasResult);
  enable();
  tick();
  assert(mock::modelCreated == 2 && mock::allocatedBytes() == 219136 && state.hasResult);

  reset();
  enable();
  mock::failAllocationAt = 2; // JPEG succeeds, RGB fails: no partial allocation leak.
  tick();
  checkReleased();
  assert(state.enabled && state.error[0] && mock::modelCreated == 0);
  mock::failAllocationAt = 0;
  tick();
  assert(state.hasResult && !state.error[0] && mock::allocatedBytes() == 219136);

  reset();
  enable();
  mock::failAllocationAt = 1; // JPEG fails, RGB succeeds: also rolls back.
  tick();
  checkReleased();
  mock::failAllocationAt = 0;
  tick();
  assert(state.hasResult);

  reset();
  enable();
  mock::runHook = [] { assert(vision_service::setEnabled(false)); };
  tick();
  checkReleased();
  assert(mock::modelRuns == 1 && mock::modelDestroyed == 1);
  assert(!state.hasResult && !state.personDetected && !state.error[0]);

  reset();
  enable();
  mock::gateHook = [] { assert(vision_service::setEnabled(false)); };
  tick();
  checkReleased();
  assert(mock::modelCreated == 0 && mock::modelRuns == 0 && !state.hasResult);

  reset();
  enable();
  mock::captureHook = [] { assert(vision_service::setEnabled(false)); };
  tick();
  checkReleased();
  assert(mock::framesReturned == 1 && mock::decodeCalls == 0 && !state.hasResult);

  reset();
  enable();
  mock::runHook = [] { assert(vision_service::setBatteryEnabled(true)); };
  tick();
  assert(!state.hasResult && mock::modelCreated == 1 && mock::modelDestroyed == 0);
  mock::runHook = nullptr;
  tick();
  assert(state.hasResult && state.batteryDetected && !state.personDetected);
  assert(mock::modelCreated == 2 && mock::modelDestroyed == 1);
  assert(mock::allocationCalls == 2); // Mode changes reuse the two frame buffers.

  reset();
  enable();
  mock::failAllocationAt = 2;
  mock::allocateHook = [] { assert(vision_service::setEnabled(false)); };
  tick();
  checkReleased();
  assert(!state.error[0] && !state.hasResult); // Stale failures cannot overwrite STOP.

  reset();
  enable();
  mock::listening = true;
  tick();
  // Long Live conversations must not starve a mission's local detector.
  assert(state.hasResult && mock::modelRuns == 1);
  mock::listening = false;
  tick();
  assert(state.hasResult);
  assert(vision_service::setEnabled(false));
  mock::listening = true;
  tick();
  checkReleased(); // Speech listening must not postpone disabled cleanup.

  std::puts("Vision memory lifecycle tests passed");
}
