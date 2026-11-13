const { test } = require('node:test');
const assert = require('node:assert/strict');
const { marginClass, margins, key } = require('../../interface/link-quality.js');

test('10 and 20 dB boundaries match the legend', () => {
  for (const value of [-20, 0, 9.99]) assert.equal(marginClass(value), 'poor');
  for (const value of [10, 15, 19.99]) assert.equal(marginClass(value), 'fair');
  for (const value of [20, 40]) assert.equal(marginClass(value), 'good');
});

test('missing margins never appear strong or become zero', () => {
  for (const value of [undefined, null, NaN, Infinity, '20']) assert.equal(marginClass(value), 'stale');
});

test('a link uses its weaker direction regardless of endpoint order', () => {
  const values = margins([[8, 1, 21, 9], [2, 3, 20, 15], [4, 5, 30, 25], [6, 7, null, 30]]);
  assert.equal(marginClass(values.get(key(1, 8))), 'poor');
  assert.equal(marginClass(values.get(key(3, 2))), 'fair');
  assert.equal(marginClass(values.get(key(5, 4))), 'good');
  assert.equal(marginClass(values.get(key(7, 6))), 'stale');
  assert.equal(margins(undefined).size, 0);
});
