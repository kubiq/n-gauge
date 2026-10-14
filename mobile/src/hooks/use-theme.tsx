import { File, Paths } from 'expo-file-system';
import { createContext, type ReactNode, useContext, useState } from 'react';
import { useColorScheme } from 'react-native';

import { ColorsContext, THEMES, type ThemeChoice } from '@/theme';

// The theme picked in Settings, kept on the phone. Read synchronously at
// start, so the first frame is already in it - no flash of the default.
// 'system' follows the phone's light/dark setting, live.
const file = new File(Paths.document, 'theme.txt');

function loadChoice(): ThemeChoice {
  try {
    const v = file.exists ? file.textSync().trim() : '';
    return v === 'system' || v in THEMES ? (v as ThemeChoice) : 'orange';
  } catch {
    return 'orange';
  }
}

type Ctx = { choice: ThemeChoice; setChoice: (c: ThemeChoice) => void };
const ChoiceContext = createContext<Ctx>({ choice: 'orange', setChoice: () => {} });

export function ThemeChoiceProvider({ children }: { children: ReactNode }) {
  const [choice, setChoiceState] = useState<ThemeChoice>(loadChoice);
  const phone = useColorScheme();
  const id = choice === 'system' ? (phone === 'light' ? 'light' : 'orange') : choice;
  const setChoice = (c: ThemeChoice) => {
    file.write(c);
    setChoiceState(c);
  };
  return (
    <ChoiceContext.Provider value={{ choice, setChoice }}>
      <ColorsContext.Provider value={THEMES[id].colors}>{children}</ColorsContext.Provider>
    </ChoiceContext.Provider>
  );
}

export function useThemeChoice(): Ctx {
  return useContext(ChoiceContext);
}
