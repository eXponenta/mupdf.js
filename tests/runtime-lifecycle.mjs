import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import * as mupdf from '../dist/mupdf.js'

assert.equal(typeof global.gc, 'function', 'Run with node --expose-gc')
const globals = Object.keys(globalThis).filter(k => k.startsWith('$libmupdf_'))
const wasm = new WebAssembly.Module(readFileSync(new URL('../dist/mupdf-wasm.wasm', import.meta.url)))
let instances = 0
let memoryRef
const options = {
 instantiateWasm(imports, receive) {
  ++instances
  const instance = new WebAssembly.Instance(wasm, imports)
  const memory = Object.values(instance.exports).find(v => v instanceof WebAssembly.Memory)
  assert.ok(memory, 'WASM exports its memory')
  memoryRef = new WeakRef(memory)
  receive(instance, wasm)
  return instance.exports
 }
}
assert.equal(instances, 0)
await assert.rejects(mupdf.init({ wasmBinary: new Uint8Array([0]), printErr() {} }))
assert.throws(() => mupdf.ColorSpace.DeviceRGB, /init/)
for (let cycle = 0; cycle < 3; ++cycle) {
 const init = mupdf.init(options)
 assert.equal(init, mupdf.init(options), 'Concurrent init is shared')
 assert.throws(() => mupdf.destroy(), /pending/)
 await init
 assert.equal(instances, cycle + 1)
 const old = new mupdf.Buffer('hello')
 assert.equal(old.asString(), 'hello')
 const path = new mupdf.Path()
 path.moveTo(1, 2)
 let moved = false
 path.walk({ moveTo(x, y) { assert.deepEqual([x, y], [1, 2]); moved = true } })
 assert.ok(moved, 'C to JS callbacks use Module')
 path.destroy()
 let closed = 0
 const stream = new mupdf.Stream({ fileSize: () => 0, read: () => 0, close: () => { ++closed } })

 const doc = new mupdf.PDFDocument()
 const obj = doc.addPage([0, 0, 20, 20], 0, {}, '')
 doc.insertPage(-1, obj)
 const page = doc.loadPage(0)
 const pixmap = page.toPixmap(mupdf.Matrix.identity, mupdf.ColorSpace.DeviceRGB, false)
 assert.ok(pixmap.asPNG().length > 0)
 // Keep JS wrappers alive: they must not keep their old WASM runtime alive.
 mupdf.destroy()
 assert.equal(closed, 1, 'Open stream handles are closed')
 stream.destroy()
 mupdf.destroy()
 assert.throws(() => old.asString())
 for (let i = 0; i < 30; ++i) {
  await new Promise(resolve => setImmediate(resolve))
  global.gc()
 }
 assert.equal(memoryRef.deref(), undefined, 'Destroyed WASM memory is collected')
 old.destroy()
 doc.destroy()
 pixmap.destroy()
}
await mupdf.init(options)
const stale = new mupdf.Buffer('old')
mupdf.destroy()
await mupdf.init(options)
assert.throws(() => stale.asString(), /destroyed/)
stale.destroy()
assert.equal(new mupdf.Buffer('new').asString(), 'new')
mupdf.destroy()
await mupdf.init()
assert.equal(new mupdf.Buffer('default loader').asString(), 'default loader')
mupdf.destroy()
assert.deepEqual(Object.keys(globalThis).filter(k => k.startsWith('$libmupdf_')), globals)
console.log('PASS: lazy init, concurrent init, render, destroy/reinit, stale objects, WASM memory GC, no globals')
