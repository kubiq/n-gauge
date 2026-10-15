// Czech, from the English text (index.ts). One file per part of the app,
// merged here; a plural is "one|other" -> "1|2-4|5+" (see tn()).
// .ts extensions: the node tests load this too.

import { app } from './cs/app.ts';
import { gauge } from './cs/gauge.ts';
import { lib } from './cs/lib.ts';
import { trips } from './cs/trips.ts';

export const cs: Record<string, string> = { ...lib, ...app, ...gauge, ...trips };
