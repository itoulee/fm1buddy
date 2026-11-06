// The case, made for your battery: the case's OpenSCAD file run in the browser (OpenSCAD in WebAssembly), shown in
// 3D (three.js: drag to turn it) and redrawn as the battery's size changes, and its STLs to download. The file is
// main's, copied next to the page by the pages workflow (fw/), or from GitHub.
// OpenSCAD and three.js load once the case is on screen

const OPENSCAD = 'https://cdn.jsdelivr.net/npm/openscad-wasm-prebuilt@1.2.0/dist/openscad.js'
const SCAD = ['fw/fm1buddy-case.scad', 'https://raw.githubusercontent.com/konsumer/fm1buddy/main/case/fm1buddy-case.scad']
const ROOM = [2, 2, 0.5] // added to the cell's size, for its tolerance and its wires

// the boards the case is made for (the case's board), and the most room there is under each (the case's BAT_MAX)
const BOARDS = {
  s3: { size: '2.8', max: [72, 53] },
  's3-35': { size: '3.5', max: [88, 57] },
  's3-4': { size: '4', max: [98, 64] }
}

// the cells' own sizes (length, width, thickness): common and cheap
const PRESETS = [
  { name: 'No battery', size: null },
  { name: '503035 · 500 mAh', size: [35, 30, 5] },
  { name: '603450 · 1200 mAh', size: [50, 34, 6] },
  { name: '103450 · 2000 mAh', size: [50, 34, 10] },
  { name: 'MakerFocus 3000 mAh', size: [65, 36, 10] },
  { name: '105060 · 3000 mAh', size: [60, 50, 10] }
]

// the pieces of the view, and their colors (the installer page's)
const PIECES = [
  { part: 'base', color: 0x36b37e },
  { part: 'view_lid', color: 0xff8a3d, fixed: true }, // the lid is the same for every battery
  { part: 'view_board', color: 0x26262b },
  { part: 'view_battery', color: 0xb8bcc4, battery: true }
]

let openscad, scadText
let setBoard = () => {}

// the case for a board (s3, s3-35, s3-4): the installer's switch picks it here too
export function pickCase(id) {
  setBoard(id)
}

async function scad() {
  if (!scadText) {
    for (const u of SCAD) {
      const r = await fetch(u).catch(() => null)
      if (r && r.ok) {
        scadText = await r.text()
        break
      }
    }
    if (!scadText) {
      throw new Error("couldn't get the case's OpenSCAD file")
    }
  }
  return scadText
}

// one part as an STL. A new OpenSCAD each time: its main runs once (a new one starts in a moment)
async function render(defines) {
  openscad ||= await import(OPENSCAD)
  const log = []
  const o = await openscad.createOpenSCAD({ noInitialRun: true, print: (s) => log.push(s), printErr: (s) => log.push(s) })
  const m = o.getInstance()
  m.FS.writeFile('/case.scad', await scad())
  const args = ['/case.scad', '--backend=manifold', '--export-format=binstl', '-o', '/out.stl']
  for (const [k, v] of Object.entries(defines)) {
    args.push('-D', `${k}=${v}`)
  }
  let code
  try {
    code = m.callMain(args)
  } catch (e) {
    code = e?.status ?? 1
  }
  let stl
  try {
    stl = m.FS.readFile('/out.stl')
  } catch {}
  if (code || !stl) {
    const why = log.find((l) => /ERROR|Assertion/.test(l)) || log.at(-1) || 'OpenSCAD failed'
    throw new Error(why.replace(/^ERROR:\s*/, '').replace(/ in file .*$/, ''))
  }
  return stl
}

// --------------------------------------------------------------------------------------------- the view ---
async function viewer(canvas) {
  const THREE = await import('three')
  const { OrbitControls } = await import('three/addons/controls/OrbitControls.js')
  const { STLLoader } = await import('three/addons/loaders/STLLoader.js')
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true })
  renderer.setPixelRatio(Math.min(devicePixelRatio, 2))
  const scene = new THREE.Scene()
  const camera = new THREE.PerspectiveCamera(35, 1, 1, 2000)
  camera.up.set(0, 0, 1) // OpenSCAD's z is up
  camera.position.set(0, -200, 100) // (not on the controls' target, or they turn it into NaNs)
  scene.add(new THREE.HemisphereLight(0xffffff, 0x444450, 2.2))
  const sun = new THREE.DirectionalLight(0xffffff, 1.6)
  sun.position.set(-60, -100, 160)
  scene.add(sun)
  const controls = new OrbitControls(camera, canvas)
  controls.enableDamping = true
  controls.enablePan = false
  const group = new THREE.Group()
  scene.add(group)
  const meshes = {}
  const loader = new STLLoader()
  let framed = false

  const size = () => {
    const w = canvas.clientWidth
    const h = canvas.clientHeight
    if (!w || !h) {
      return // (hidden)
    }
    if (canvas.width !== Math.round(w * renderer.getPixelRatio()) || canvas.height !== Math.round(h * renderer.getPixelRatio())) {
      renderer.setSize(w, h, false)
      camera.aspect = w / h
      camera.updateProjectionMatrix()
      if (framed) {
        frame()
      }
    }
  }
  // aim at it all, far enough that its bounding sphere fits however it's turned: from where the camera is looking
  // now, or the first time, from the front, above and to one side
  const frame = () => {
    const box = new THREE.Box3().setFromObject(group)
    if (box.isEmpty()) {
      return // (nothing there yet)
    }
    const sphere = box.getBoundingSphere(new THREE.Sphere())
    const fov = (camera.fov * Math.PI) / 180
    const fit = Math.min(fov, 2 * Math.atan(Math.tan(fov / 2) * camera.aspect))
    let dir = camera.position.clone().sub(controls.target).normalize()
    if (!framed || !Number.isFinite(dir.x)) {
      dir = new THREE.Vector3(1.1, -2.3, 1.5).normalize()
    }
    controls.target.copy(sphere.center)
    camera.position.copy(sphere.center).addScaledVector(dir, (sphere.radius / Math.sin(fit / 2)) * 1.04)
    controls.update()
    framed = true
  }
  renderer.setAnimationLoop(() => {
    size()
    controls.update()
    renderer.render(scene, camera)
  })
  return {
    has: (name) => !!meshes[name],
    // aim at it all (after it changed: a taller base)
    fit() {
      if (meshes.base) {
        frame()
      }
    },
    set(name, stl, color) {
      if (meshes[name]) {
        group.remove(meshes[name])
        meshes[name].geometry.dispose()
        delete meshes[name]
      }
      if (!stl) {
        return
      }
      const geo = loader.parse(stl.buffer.slice(stl.byteOffset, stl.byteOffset + stl.byteLength))
      geo.computeVertexNormals()
      const mesh = new THREE.Mesh(geo, new THREE.MeshStandardMaterial({ color, roughness: 0.6, metalness: 0.05 }))
      meshes[name] = mesh
      group.add(mesh)
    }
  }
}

// ------------------------------------------------------------------------------------------------ page ---
export function init() {
  const box = document.getElementById('maker')
  if (!box) {
    return
  }
  const $ = (s) => box.querySelector(s)
  const inputs = ['x', 'y', 'z'].map((n) => $(`[name=${n}]`))
  const sw = $('[name=sw]')
  const btn = $('[name=btn]')
  const status = $('.maker-status')
  const canvas = $('canvas')
  const still = $('.case-view img')
  const baseBtn = $('.maker-base')
  const lidBtn = $('.maker-lid')
  let none = false
  let board = 's3'
  let view, baseStl, lidUrl, lidBoard, viewLidBoard, baseUrl, timer, busy, again

  const say = (text, bad) => {
    status.textContent = text
    status.className = `note maker-status${bad ? ' bad' : ''}`
  }

  // what the case is made with now; or an error (a size that doesn't fit)
  const params = () => {
    const b = BOARDS[board]
    const MAX = b.max
    if (none) {
      return { defines: { board: `"${b.size}"`, battery: false, button_holes: btn.checked }, name: `fm1buddy-case-${board}${btn.checked ? '-buttons' : ''}-base.stl` }
    }
    const size = inputs.map((i) => parseFloat(i.value))
    if (size.some((v) => !(v > 0))) {
      return { error: 'Fill in the battery’s length, width and thickness.' }
    }
    const bat = size.map((v, k) => +(v + ROOM[k]).toFixed(2))
    if (bat[0] > MAX[0] || bat[1] > MAX[1]) {
      return { error: `That’s too big: a battery up to ${MAX[0] - ROOM[0]} × ${MAX[1] - ROOM[1]} mm fits (any thickness).` }
    }
    return {
      defines: { board: `"${b.size}"`, battery: true, bat: `[${bat}]`, switch_hole: sw.checked ? '[9, 4.2]' : '[0, 0]', button_holes: btn.checked },
      bat,
      name: `fm1buddy-case-${board}-battery-${size.join('x')}${sw.checked ? '-switch' : ''}${btn.checked ? '-buttons' : ''}-base.stl`
    }
  }

  // make the base (and the view's pieces) for what's picked; once at a time, the last change wins
  const make = async () => {
    if (busy) {
      again = true
      return
    }
    const p = params()
    if (p.error) {
      say(p.error, true)
      baseBtn.removeAttribute('href')
      baseBtn.setAttribute('aria-disabled', 'true')
      return
    }
    busy = true
    baseBtn.setAttribute('aria-disabled', 'true')
    say(openscad ? 'Making it…' : 'Getting OpenSCAD (about 11 MB, once)…')
    try {
      view ||= await viewer(canvas)
      for (const piece of PIECES) {
        if (piece.fixed && view.has(piece.part) && viewLidBoard === board) {
          continue // (made the first time, for this board)
        }
        if (piece.fixed) {
          viewLidBoard = board
        }
        let stl = null
        if (!(piece.battery && none)) {
          // (only the base has to work: the view's other pieces are only to look at)
          stl = await render({ ...p.defines, part: `"${piece.part}"` }).catch((e) => {
            if (piece.part === 'base') {
              throw e
            }
            return null
          })
        }
        if (piece.part === 'base') {
          baseStl = stl
        }
        view.set(piece.part, stl, piece.color)
      }
      view.fit()
      still.hidden = true
      canvas.hidden = false
      if (baseUrl) {
        URL.revokeObjectURL(baseUrl)
      }
      baseUrl = URL.createObjectURL(new Blob([baseStl], { type: 'model/stl' }))
      baseBtn.href = baseUrl
      baseBtn.download = p.name
      baseBtn.removeAttribute('aria-disabled')
      if (lidBoard !== board) {
        // (the lid made for another board)
        if (lidUrl) {
          URL.revokeObjectURL(lidUrl)
        }
        lidUrl = null
        lidBtn.removeAttribute('href')
        lidBtn.download = `fm1buddy-case-${board}-lid.stl`
      }
      lidBtn.removeAttribute('aria-disabled')
      say(none ? 'Without a battery. Drag it to turn it.' : `Room for ${p.bat.join(' × ')} mm: the case is ${(p.bat[2] + 0.7).toFixed(1)} mm taller than without a battery. Drag it to turn it.`)
    } catch (e) {
      say(`It didn’t work: ${e.message}`, true)
    } finally {
      busy = false
      if (again) {
        again = false
        make()
      }
    }
  }
  const changed = () => {
    for (const b of box.querySelectorAll('.maker-presets button')) {
      const p = PRESETS[b.dataset.i]
      b.classList.toggle('on', p.size ? !none && p.size.every((v, k) => +inputs[k].value === v) : none)
    }
    for (const i of inputs) {
      i.disabled = none
    }
    sw.disabled = none
    clearTimeout(timer)
    timer = setTimeout(make, 300)
  }
  const pick = (p) => {
    none = !p.size
    if (p.size) {
      p.size.forEach((v, k) => (inputs[k].value = v))
    }
    changed()
  }

  $('.maker-presets').innerHTML = PRESETS.map((p, i) => `<button type="button" data-i="${i}">${p.name}</button>`).join('')
  for (const b of box.querySelectorAll('.maker-presets button')) {
    b.onclick = () => pick(PRESETS[b.dataset.i])
  }
  for (const i of [...inputs, sw, btn]) {
    i.oninput = changed
  }

  // the board switch: the case for that board (and its picture, until the 3D view is up)
  const sizes = [...box.querySelectorAll('.maker-board button')]
  setBoard = (id) => {
    if (!BOARDS[id] || id === board) {
      return
    }
    board = id
    for (const b of sizes) {
      b.setAttribute('aria-checked', String(b.dataset.board === id))
    }
    still.src = `case/${id}.png`
    inputs[0].max = BOARDS[id].max[0] - ROOM[0]
    inputs[1].max = BOARDS[id].max[1] - ROOM[1]
    if (started) {
      changed()
    }
  }
  for (const b of sizes) {
    b.onclick = () => setBoard(b.dataset.board)
  }

  // the lid to print (flat side down): made when it's asked for
  lidBtn.onclick = async (e) => {
    if (lidUrl || lidBtn.getAttribute('aria-disabled')) {
      return
    }
    e.preventDefault()
    lidBtn.setAttribute('aria-disabled', 'true')
    try {
      const made = board
      const stl = await render({ board: `"${BOARDS[made].size}"`, part: '"lid"' })
      if (made !== board) {
        lidBtn.removeAttribute('aria-disabled')
        return // (the board changed meanwhile)
      }
      lidBoard = made
      lidUrl = URL.createObjectURL(new Blob([stl], { type: 'model/stl' }))
      lidBtn.href = lidUrl
      lidBtn.removeAttribute('aria-disabled')
      lidBtn.click()
    } catch (err) {
      say(`It didn’t work: ${err.message}`, true)
      lidBtn.removeAttribute('aria-disabled')
    }
  }

  // start with no battery, once the case is on screen
  none = true
  inputs.forEach((i, k) => (i.value = PRESETS[4].size[k]))
  let started = false
  new IntersectionObserver(
    (seen, o) => {
      if (seen.some((s) => s.isIntersecting)) {
        o.disconnect()
        started = true
        changed()
      }
    },
    { rootMargin: '300px' }
  ).observe(box)
}
