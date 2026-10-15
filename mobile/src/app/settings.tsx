import { AboutCard } from '@/components/about-card';
import { LanguageCard } from '@/components/language-card';
import { LogKeyCard } from '@/components/log-key-card';
import { LogsFolderCard } from '@/components/logs-folder-card';
import { PlacesCard } from '@/components/places-card';
import { ThemeCard } from '@/components/theme-card';
import { Screen } from '@/components/ui';

// This phone's settings: the language, the theme, the log key, the logs
// folder and your places, all kept on the phone and usable off the gauge's
// WiFi too. The gauge's own are a screen of their own (gauge-settings.tsx),
// reached from the Gauge screen.
export default function SettingsScreen() {
  return (
    <Screen>
      <LanguageCard />
      <ThemeCard />
      <LogKeyCard />
      <LogsFolderCard />
      <PlacesCard />
      <AboutCard />
    </Screen>
  );
}
