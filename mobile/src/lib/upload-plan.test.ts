/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { planUpload } from './upload-plan.ts';

test('a new file goes up from 0', () => {
  assert.deepEqual(planUpload({ uploaded: 0, pending: 500 }, undefined), { kind: 'send', offset: 0 });
});

test('a grown file sends only its new tail', () => {
  assert.deepEqual(planUpload({ uploaded: 1000, pending: 200 }, 1000), { kind: 'send', offset: 1000 });
});

test('nothing waiting, nothing to do', () => {
  assert.deepEqual(planUpload({ uploaded: 1000, pending: 0 }, 1000), { kind: 'none' });
});

// The last PUT went through but its reply never came back.
test('server already has it all: just record it', () => {
  assert.deepEqual(planUpload({ uploaded: 1000, pending: 200 }, 1200), { kind: 'done', size: 1200 });
});

test('server has part of the tail: continue from there', () => {
  assert.deepEqual(planUpload({ uploaded: 1000, pending: 200 }, 1150), { kind: 'send', offset: 1150 });
});

test('server behind what we recorded (it lost data): resend from where it is', () => {
  assert.deepEqual(planUpload({ uploaded: 1000, pending: 200 }, 400), { kind: 'send', offset: 400 });
  assert.deepEqual(planUpload({ uploaded: 1000, pending: 200 }, undefined), { kind: 'send', offset: 0 });
});

// The pod's copy shrank, so the pull restarted it from 0.
test('server holds more than we do: replace it', () => {
  assert.deepEqual(planUpload({ uploaded: 0, pending: 50 }, 8000), { kind: 'send', offset: 0 });
  assert.deepEqual(planUpload({ uploaded: 1000, pending: 200 }, 5000), { kind: 'send', offset: 0 });
});
