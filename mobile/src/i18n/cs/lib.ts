// Czech for the lib part - see ../index.ts. English text -> Czech.
export const lib: Record<string, string> = {
  // Sync status and errors (hooks/use-sync.tsx)
  'Pulled 1 file.|Pulled {n} files.': 'Stažen {n} soubor.|Staženy {n} soubory.|Staženo {n} souborů.',
  'Cancelled after {done} of {total}.': 'Zrušeno po {done} z {total}.',
  'Stopped after {done} of {total}: {error}': 'Zastaveno po {done} z {total}: {error}',
  // Leaving the gauge's WiFi after a sync (a toast)
  '1 new file from the gauge - disconnected.|{n} new files from the gauge - disconnected.':
    '{n} nový soubor z budíku - odpojeno.|{n} nové soubory z budíku - odpojeno.|{n} nových souborů z budíku - odpojeno.',
  'Nothing new on the gauge - disconnected.': 'Na budíku nic nového - odpojeno.',

  // The connection (hooks/use-pod.tsx)
  'Lost the gauge’s WiFi': 'Spojení s Wi-Fi budíku ztraceno',

  // Talking to the gauge (lib/pod.ts)
  'The gauge did not answer': 'Budík neodpovídá',

  // Reading a log (lib/logtext.ts)
  'Encrypted - this phone has no log key (Settings, Log encryption).':
    'Šifrovaný - tento telefon nemá klíč k záznamům (Nastavení, Šifrování záznamů).',
  'Not an encrypted log, despite its name.': 'Podle názvu šifrovaný záznam, ale není.',
  'Encrypted to a different key than this phone’s - import that key to read it.':
    'Šifrovaný jiným klíčem, než má tento telefon - pro přečtení ten klíč importujte.',

  // Sharing and saving to a folder (lib/share.ts)
  'Sharing is not available on this device': 'Sdílení není na tomto zařízení dostupné',
  'Share the track': 'Sdílet trasu',
  'Share the car data': 'Sdílet data z auta',
  'Successfully synced.': 'Úspěšně synchronizováno.',
  'Not saved:': 'Neuloženo:',
  'Saving {at} of {of}: {name}': 'Ukládám {at} z {of}: {name}',

  // Pulls (lib/store.ts)
  '{name}: planned from {from}, but the inbox ends at {end}':
    '{name}: plánováno od {from}, ale soubor v telefonu končí na {end}',
};
