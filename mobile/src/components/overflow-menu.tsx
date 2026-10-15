import { DropdownMenu, DropdownMenuItem, Host, Icon, IconButton, Text } from '@expo/ui/jetpack-compose';
import { useState } from 'react';

import { t } from '@/i18n';
import { useColors } from '@/theme';

export type MenuItem = { label: string; onPress: () => void; disabled?: boolean };

// The ⋮ menu in a screen's header, for the rarer actions - the native
// Material dropdown (Jetpack Compose, via @expo/ui), as other Android apps
// have it.
export function OverflowMenu({ items }: { items: MenuItem[] }) {
  const [open, setOpen] = useState(false);
  const C = useColors();
  if (!items.length) return null;
  return (
    <Host matchContents>
      <DropdownMenu expanded={open} onDismissRequest={() => setOpen(false)} color={C.card}>
        <DropdownMenu.Trigger>
          <IconButton onClick={() => setOpen(true)}>
            <Icon source={require('@/assets/icons/more_vert.xml')} tint={C.text} size={24} contentDescription={t('More')} />
          </IconButton>
        </DropdownMenu.Trigger>
        <DropdownMenu.Items>
          {items.map((it) => (
            <DropdownMenuItem
              key={it.label}
              enabled={!it.disabled}
              elementColors={{ textColor: C.text, disabledTextColor: C.dim }}
              onClick={() => {
                setOpen(false);
                it.onPress();
              }}>
              <DropdownMenuItem.Text>
                <Text>{it.label}</Text>
              </DropdownMenuItem.Text>
            </DropdownMenuItem>
          ))}
        </DropdownMenu.Items>
      </DropdownMenu>
    </Host>
  );
}
