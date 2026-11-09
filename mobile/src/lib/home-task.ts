// The home server sync in the background (asked 2026-11-06): with the app
// closed, about every hour but not at night, Android's WorkManager runs it (expo-background-
// task; 15 minutes is the shortest it allows, and it waits for a network).
// The same sync as the app's (home-sync.ts), into the same inbox - so a
// drive the gauge uploaded as the car started is on the phone by the time
// it is opened.
//
// Defined at module scope, as TaskManager wants: imported by the root layout,
// so it is there however the JS starts - the app opened or the task woken.

import * as BackgroundTask from 'expo-background-task';
import * as TaskManager from 'expo-task-manager';
import { AppState } from 'react-native';

import { updateKnownBeacons } from './beacon';
import { loadHome } from './home';
import { syncFromHome } from './home-sync';
import { saveNewLogs } from './share';
import { loadAutoSave } from './store';

const TASK = 'home-server-sync';
const EVERY_MIN = 60;
// Not at night, phone time (asked 2026-11-06: no drives then, so nothing
// new to pull). WorkManager has no time windows: it still wakes the task
// each hour, and the task goes back to sleep at once - no network, no work.
const NIGHT_FROM_H = 23;
const NIGHT_TO_H = 6;

TaskManager.defineTask(TASK, async () => {
  const h = new Date().getHours();
  if (h >= NIGHT_FROM_H || h < NIGHT_TO_H) return BackgroundTask.BackgroundTaskResult.Success;
  // Open in front, the app syncs by itself (use-sync.tsx) and shows it.
  if (AppState.currentState === 'active') return BackgroundTask.BackgroundTaskResult.Success;
  try {
    const r = await syncFromHome();
    if (r.state === 'done' && r.files) {
      // The beacon's "a drive is waiting" note goes, now it is here.
      await updateKnownBeacons().catch(() => undefined);
      if (loadAutoSave()) await saveNewLogs().catch(() => undefined);
      // No note that they came (removed 2026-11-09): they always do now,
      // by themselves - one each drive was noise.
    }
    return BackgroundTask.BackgroundTaskResult.Success;
  } catch (e) {
    console.warn('background home sync', e);
    return BackgroundTask.BackgroundTaskResult.Failed;
  }
});

/** Hourly while a home server is set up (the gauge's Upload card); off without one. */
export async function scheduleHomeTask(): Promise<void> {
  const on = !!(await loadHome());
  const registered = await TaskManager.isTaskRegisteredAsync(TASK);
  if (on && !registered) await BackgroundTask.registerTaskAsync(TASK, { minimumInterval: EVERY_MIN });
  else if (!on && registered) await BackgroundTask.unregisterTaskAsync(TASK);
}
