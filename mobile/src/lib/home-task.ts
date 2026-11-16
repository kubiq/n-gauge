// The home server sync in the background (asked 2026-11-06): with the app
// closed, every few hours (EVERY_MIN) but not at night - and not at all with
// Settings' switch off - Android's WorkManager runs it (expo-background-
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
import { loadAutoSave, loadHomeBackground, loadHomeTaskEvery, saveHomeTaskEvery } from './store';

const TASK = 'home-server-sync';
// Every 4 hours (asked 2026-11-16, for the battery - was hourly): a drive
// reaches the server only with the next drive's upload anyway, so sooner
// gains little. Opening the app syncs at once besides.
const EVERY_MIN = 240;
// Not at night, phone time (asked 2026-11-06: no drives then, so nothing
// new to pull). WorkManager has no time windows: it still wakes the task
// at night, and the task goes back to sleep at once - no network, no work.
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

/**
 * Every EVERY_MIN while a home server is set up (the gauge's Upload card) and
 * the Settings switch is on; off otherwise. Registered again when the
 * interval changed (an app update): WorkManager keeps the one it was given.
 */
export async function scheduleHomeTask(): Promise<void> {
  const on = !!(await loadHome()) && loadHomeBackground();
  const registered = await TaskManager.isTaskRegisteredAsync(TASK);
  if (!on) {
    if (registered) await BackgroundTask.unregisterTaskAsync(TASK);
    return;
  }
  if (registered && loadHomeTaskEvery() === EVERY_MIN) return;
  if (registered) await BackgroundTask.unregisterTaskAsync(TASK);
  await BackgroundTask.registerTaskAsync(TASK, { minimumInterval: EVERY_MIN });
  saveHomeTaskEvery(EVERY_MIN);
}
