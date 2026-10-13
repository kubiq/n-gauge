import { Host, Icon } from '@expo/ui/jetpack-compose';

import { C } from '@/theme';

/** An encrypted log: dim, or red when this phone cannot read it. */
export function LockIcon({ bad = false, size = 16 }: { bad?: boolean; size?: number }) {
  return (
    <Host matchContents>
      <Icon
        source={require('@/assets/icons/lock.xml')}
        tint={bad ? C.bad : C.dim}
        size={size}
        contentDescription={bad ? 'Encrypted, not readable here' : 'Encrypted'}
      />
    </Host>
  );
}
