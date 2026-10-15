// Czech for the lib part - see ../index.ts. English text -> Czech.
export const lib: Record<string, string> = {
  // Sync status and errors (hooks/use-sync.tsx); the progress line also for uploads (lib/store.ts)
  '{at}/{of} {name}: {got} of {total}': '{at}/{of} {name}: {got} z {total}',
  'Pulled 1 file.|Pulled {n} files.': 'Stažen {n} soubor.|Staženy {n} soubory.|Staženo {n} souborů.',
  'Cancelled after {done} of {total}.': 'Zrušeno po {done} z {total}.',
  'Stopped after {done} of {total}: {error}': 'Zastaveno po {done} z {total}: {error}',

  // The connection (hooks/use-pod.tsx)
  'Lost the gauge’s WiFi': 'Spojení s Wi-Fi budíku ztraceno',

  // Talking to the gauge (lib/pod.ts)
  'The gauge did not answer': 'Budík neodpovídá',

  // The upload server (lib/backend.ts)
  'The server refused the token': 'Server token odmítl',
  'The server did not answer - is the VPN up?': 'Server neodpovídá - běží VPN?',
  'Cannot reach the server: {error}': 'Server je nedostupný: {error}',
  'Upload failed - is the VPN up? ({error})': 'Nahrání selhalo - běží VPN? ({error})',

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
  '{n} saved': 'Uloženo: {n}',
  '{n} already there': 'přeskočeno (už tam jsou): {n}',
  'Not saved:': 'Neuloženo:',
  'Saving {at} of {of}: {name}': 'Ukládám {at} z {of}: {name}',

  // Pulls and uploads (lib/store.ts)
  '{name}: planned from {from}, but the inbox ends at {end}':
    '{name}: plánováno od {from}, ale soubor v telefonu končí na {end}',
  '{name}: server size {size}, expected {expected}': '{name}: velikost na serveru {size}, očekáváno {expected}',
  '{name}: changed on the server meanwhile - try again': '{name}: na serveru se mezitím změnil - zkuste to znovu',
};
