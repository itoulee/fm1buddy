// The fm1buddy installer page. The firmware is the latest release's, copied next to the page by the pages
// workflow (fw/): GitHub's release downloads can't be fetched from another site, its API can.

import { init as initMap } from './midimap.js'
import { init as initMaker } from './casemaker.js'

const REPO = 'konsumer/fm1buddy'
// The screens the firmware is made for: the page's switch picks one, and installs its .bin (a release from before a
// board existed has no manifest for it)
const BOARDS = [
  { id: 's3', name: '2.8"', bin: 'fw/fm1buddy-cyd-s3.bin', manifest: 'fw/manifest.json' },
  { id: 's3-35', name: '3.5"', bin: 'fw/fm1buddy-cyd-s3-35.bin', manifest: 'fw/manifest-35.json' },
  { id: 's3-4', name: '4"', bin: 'fw/fm1buddy-cyd-s3-4.bin', manifest: 'fw/manifest-4.json' }
]
const FW = { version: 'fw/version.json' }

const $ = (s) => document.querySelector(s)
const $$ = (s) => [...document.querySelectorAll(s)]

// ------------------------------------------------------------------------------------------- release ---
async function json(url) {
  const r = await fetch(url, { cache: 'no-cache' })
  if (!r.ok) {
    throw new Error(r.status)
  }
  return r.json()
}

function noFirmware(sel) {
  for (const el of $$(sel)) {
    el.setAttribute('aria-disabled', 'true')
    el.disabled = true
    el.removeAttribute('href')
  }
}

// the board the switch has on: its install button, and its .bin in the esptool line
function pickBoard(id) {
  for (const b of BOARDS) {
    $(`.board-switch [data-board="${b.id}"]`).setAttribute('aria-checked', String(b.id === id))
    $(`.fw-${b.id}`).hidden = b.id !== id
  }
}

async function release() {
  const el = $('#release')
  let here = null
  try {
    const v = await json(FW.version)
    here = v.tag
  } catch (_) {}
  let latest = null
  try {
    latest = await json(`https://api.github.com/repos/${REPO}/releases/latest`)
  } catch (_) {}

  if (!here) {
    noFirmware(BOARDS.flatMap((b) => [`.fw-${b.id} button`, `.fw-${b.id}-bin`]).join(', '))
    el.innerHTML = latest ? `The latest release, <a href="${latest.html_url}">${latest.tag_name}</a>, isn't on this page yet: get the files from there.` : `There's no release yet: <a href="https://github.com/${REPO}">build it from the source</a>.`
    return
  }
  for (const b of BOARDS) {
    const has = b.id === 's3' || (await fetch(b.manifest, { method: 'HEAD', cache: 'no-cache' }).catch(() => ({}))).ok
    if (!has) {
      // (an older release: this board's firmware isn't there)
      noFirmware(`.fw-${b.id} button, .fw-${b.id}-bin`)
      const opt = $(`.board-switch [data-board="${b.id}"]`)
      opt.setAttribute('aria-disabled', 'true')
      opt.disabled = true
      continue
    }
    $(`.fw-${b.id}-bin`).href = b.bin
    $(`.fw-${b.id}`).setAttribute('manifest', b.manifest)
  }
  if ($('.board-switch [aria-checked="true"]').getAttribute('aria-disabled') === 'true') {
    pickBoard('s3') // (the chosen board's firmware is missing: the 2.8"'s is always here)
  }

  const date = latest && latest.tag_name === here ? ` · ${new Date(latest.published_at).toLocaleDateString()}` : ''
  const url = `https://github.com/${REPO}/releases/tag/${encodeURIComponent(here)}`
  el.innerHTML = `Version <a href="${url}">${here}</a>${date}`
  if (latest && latest.tag_name !== here) {
    el.innerHTML += ` · a newer one, <a href="${latest.html_url}">${latest.tag_name}</a>, is on its way here`
  }
}

// ------------------------------------------------------------------------------------------- tabs ---
function tab() {
  const name = location.hash === '#midimap' ? 'map' : 'install'
  for (const el of $$('[data-page]')) {
    el.hidden = el.dataset.page !== name
  }
  for (const a of $$('.tabs a')) {
    a.classList.toggle('on', a.dataset.tab === name)
  }
  document.body.dataset.tab = name
}

// the board switch: the button that's picked shows its board's install button
for (const b of $$('.board-switch button')) {
  b.addEventListener('click', () => {
    if (!b.disabled) {
      pickBoard(b.dataset.board)
    }
  })
}

window.addEventListener('hashchange', tab)
tab()
initMap()
initMaker()
release()
