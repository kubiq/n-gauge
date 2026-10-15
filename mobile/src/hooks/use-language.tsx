import { useLocales } from 'expo-localization';
import { File, Paths } from 'expo-file-system';
import { createContext, Fragment, type ReactNode, useContext, useState } from 'react';

import { type Lang, type LangChoice, setLang } from '@/i18n';

// The language picked in Settings, kept on the phone: English, Czech, or
// the phone's own (Czech if the phone is in Czech, else English). Read
// synchronously at start, so the first frame is already in it.
const file = new File(Paths.document, 'language.txt');

function loadChoice(): LangChoice {
  try {
    const v = file.exists ? file.textSync().trim() : '';
    return v === 'en' || v === 'cs' ? v : 'system';
  } catch {
    return 'system';
  }
}

type Ctx = { choice: LangChoice; lang: Lang; setChoice: (c: LangChoice) => void };
const LanguageContext = createContext<Ctx>({ choice: 'system', lang: 'en', setChoice: () => {} });

export function LanguageProvider({ children }: { children: ReactNode }) {
  const [choice, setChoiceState] = useState<LangChoice>(loadChoice);
  const phone = useLocales()[0]?.languageCode === 'cs' ? 'cs' : 'en';
  const lang: Lang = choice === 'system' ? phone : choice;
  // Before anything below renders: t() reads it from the i18n module.
  setLang(lang);
  const setChoice = (c: LangChoice) => {
    file.write(c);
    setChoiceState(c);
  };
  return (
    <LanguageContext.Provider value={{ choice, lang, setChoice }}>
      {/* Keyed on the language: a change redraws everything in it - the
          screens remember nothing in t()'s words, so nothing goes stale. */}
      <Fragment key={lang}>{children}</Fragment>
    </LanguageContext.Provider>
  );
}

export function useLanguage(): Ctx {
  return useContext(LanguageContext);
}
