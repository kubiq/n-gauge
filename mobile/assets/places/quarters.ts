// The Czech cities' cadastral areas (scripts/make-quarters.ts), as an asset:
// a require of a .tsv is only its id - the file is read when needed
// (place-atlas.ts), not held as a module. Beside index.ts, which make-places
// regenerates, so it survives that.
export const QUARTERS: number = require('./quarters.tsv');
