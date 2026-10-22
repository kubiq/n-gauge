// Czech for the app part - see ../index.ts. English text -> Czech.
export const app: Record<string, string> = {
  // Screen titles (_layout.tsx)
  Settings: 'Nastavení',
  Gauge: 'Budík',
  'Gauge settings': 'Nastavení budíku',
  'Scan the gauge’s QR': 'Načíst QR kód budíku',
  Drive: 'Jízda',
  Name: 'Název',
  'Log encryption': 'Šifrování záznamů',

  // The language card (language-card.tsx); "Follow the phone" is below.
  Language: 'Jazyk',
  'Place names stay as they are on the map. The gauge’s own screen is in English.':
    'Názvy míst zůstávají, jak jsou na mapě. Displej budíku je v angličtině.',

  // Theme names (theme.ts) and the theme card (theme-card.tsx)
  'Dark – orange': 'Tmavý – oranžový',
  'Dark – N blue': 'Tmavý – N modrý',
  'Dark – red': 'Tmavý – červený',
  Light: 'Světlý',
  'Follow the phone': 'Podle telefonu',
  Theme: 'Motiv',
  'Dark is easier on the eyes in the car at night. Follow the phone: Light by day, Dark – orange by night, as the phone switches.':
    'Tmavý motiv v autě v noci méně oslňuje. Podle telefonu: přes den Světlý, v noci Tmavý – oranžový, jak telefon přepíná.',

  // The ⋮ menu (overflow-menu.tsx)
  More: 'Další',

  // About (about-card.tsx)
  About: 'O aplikaci',
  App: 'Aplikace',
  'older firmware': 'starší firmware',
  '{build}, read {when}': '{build}, zjištěno {when}',
  'Place names © GeoNames (geonames.org), CC BY 4.0.': 'Názvy míst © GeoNames (geonames.org), CC BY 4.0.',
  'City quarter boundaries © ČÚZK (RÚIAN), via ČSÚ open data, CC BY 4.0.':
    'Hranice městských částí © ČÚZK (RÚIAN), z otevřených dat ČSÚ, CC BY 4.0.',

  // Log encryption card (log-key-card.tsx), shared with log-key.tsx
  'This phone’s key': 'Klíč tohoto telefonu',
  'not set up': 'nenastaven',
  'not encrypted': 'nešifrované',
  'encrypted to this phone': 'šifrované pro tento telefon',
  'encrypted to another key': 'šifrované jiným klíčem',
  Manage: 'Spravovat',
  'Set up': 'Nastavit',

  // Trip list (trip-list-card.tsx); its switches reuse the drive screen's labels
  'Trip list': 'Seznam jízd',
  'Shown beside each drive in the list. A drive’s own screen always shows them all.':
    'Zobrazí se u každé jízdy v seznamu. Obrazovka jízdy je ukazuje vždy všechny.',

  // Gauge connection (connection-card.tsx)
  'Gauge connection': 'Připojení k budíku',
  'Disconnect after syncing': 'Po synchronizaci odpojit',
  'Once the new logs are on the phone, it leaves the gauge’s WiFi and is back on the internet. Not while the gauge’s settings or the log key are open.':
    'Jakmile jsou nové záznamy v telefonu, odpojí se od WiFi budíku a je zase na internetu. Ne, dokud jsou otevřená nastavení budíku nebo klíč záznamů.',
  // The gauge's beacon (connection-card.tsx, beacon.ts)
  'Tell me about new drives': 'Upozornit na nové jízdy',
  'Sync them when the app opens': 'Synchronizovat je po otevření aplikace',
  'Parked, the gauge’s WiFi says which drive it has last. The phone hears it in its WiFi scans, without connecting, and tells you of a drive it does not have yet - about every 15 minutes. Needs location “all the time”: Android shows WiFi scans only with it.':
    'Zaparkovaný budík ve své WiFi ohlašuje, kterou jízdu má jako poslední. Telefon to zachytí při hledání WiFi sítí, bez připojení, a upozorní na jízdu, kterou ještě nemá - zhruba každých 15 minut. Potřebuje polohu „vždy“: bez ní Android výsledky hledání WiFi nedá.',
  'New drives on the gauge': 'Nové jízdy v budíku',
  'Open the app to sync them.': 'Otevřete aplikaci a synchronizujte je.',

  // Logs folder (logs-folder-card.tsx)
  Folder: 'Složka',
  'Logs folder': 'Složka pro záznamy',
  'Save to': 'Ukládat do',
  'ask every time': 'pokaždé se zeptat',
  'Saved logs are decrypted, under their plain names; files already there are skipped.':
    'Uložené záznamy jsou dešifrované a pod svými běžnými názvy; soubory, které už ve složce jsou, se přeskočí.',
  Change: 'Změnit',
  'Choose a folder': 'Zvolit složku',
  'Ask every time': 'Pokaždé se zeptat',

  // Log encryption screen (log-key.tsx)
  'Save the key in KeePass now': 'Uložte klíč hned do KeePassu',
  'Logs encrypted to this key can only ever be read with it. If the phone is lost and the key is not saved elsewhere, they are lost too.':
    'Záznamy šifrované tímto klíčem jde přečíst jen s ním. Když telefon ztratíte a klíč nebude uložený jinde, přijdete i o ně.',
  Later: 'Později',
  'Copy it': 'Zkopírovat',
  Copied: 'Zkopírováno',
  'Paste it into KeePass now. The clipboard is cleared in a minute.':
    'Vložte ho hned do KeePassu. Schránka se za minutu vymaže.',
  'Not a key': 'Toto není klíč',
  'Paste the line starting "ngauge-log-key-v1:" from KeePass.':
    'Vložte z KeePassu řádek začínající „ngauge-log-key-v1:“.',
  'Replace this phone’s key?': 'Nahradit klíč tohoto telefonu?',
  'Logs encrypted to the current key will no longer open on this phone - only with that key. Make sure it is saved in KeePass first.':
    'Záznamy šifrované současným klíčem už na tomto telefonu nepůjdou otevřít - jen s tím klíčem. Nejdřív se ujistěte, že je uložený v KeePassu.',
  Cancel: 'Zrušit',
  Replace: 'Nahradit',
  Fingerprint: 'Otisk',
  'The gauge’s settings page shows the same fingerprint when it encrypts to this key.':
    'Stránka nastavení budíku ukazuje stejný otisk, když šifruje tímto klíčem.',
  'Copy the key for KeePass': 'Zkopírovat klíč pro KeePass',
  'With a key, the gauge encrypts its logs so that only this phone can read them - a card taken out of the gauge gives nothing away. The gauge only ever gets the public half.':
    'S klíčem budík šifruje své záznamy tak, že je přečte jen tento telefon - karta vyjmutá z budíku nic neprozradí. Budík dostane vždy jen veřejnou polovinu.',
  'Create a key': 'Vytvořit klíč',
  Import: 'Importovat',
  'Replace with a key from KeePass': 'Nahradit klíčem z KeePassu',
  'Import a key from KeePass': 'Importovat klíč z KeePassu',
  'The gauge': 'Budík',
  'Connect to the gauge (tap it at the top) to set its key.':
    'Pro nastavení klíče se připojte k budíku (klepněte na něj nahoře).',
  'Reading the gauge’s settings… (older firmware does not know about log keys)':
    'Načítám nastavení budíku… (starší firmware klíče pro záznamy nezná)',
  Logs: 'Záznamy',
  'The gauge encrypts to {fp} - not this phone’s key. Its logs only open with that key.':
    'Budík šifruje klíčem {fp} - ne klíčem tohoto telefonu. Jeho záznamy otevře jen ten klíč.',
  'Encrypt the gauge’s logs with this key': 'Šifrovat záznamy budíku tímto klíčem',
  'Stop encrypting': 'Přestat šifrovat',
  'Stop encrypting?': 'Přestat šifrovat?',
  'New logs will be plain again. Logs already encrypted stay encrypted.':
    'Nové záznamy budou opět nešifrované. Už zašifrované záznamy zůstanou šifrované.',
  Stop: 'Přestat',
  'A change applies at once: the gauge closes its open logs and starts new files.':
    'Změna platí hned: budík zavře otevřené záznamy a začne nové soubory.',
};
