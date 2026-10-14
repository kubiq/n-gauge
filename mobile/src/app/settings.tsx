import { AboutCard } from '@/components/about-card';
import { LogKeyCard } from '@/components/log-key-card';
import { LogsFolderCard } from '@/components/logs-folder-card';
import { Screen } from '@/components/ui';

// This phone's settings: the log key and the logs folder, both needed off
// the gauge's WiFi too. The gauge's own are a screen of their own
// (gauge-settings.tsx), reached from the Gauge screen.
export default function SettingsScreen() {
  return (
    <Screen>
      <LogKeyCard />
      <LogsFolderCard />
      <AboutCard />
    </Screen>
  );
}
