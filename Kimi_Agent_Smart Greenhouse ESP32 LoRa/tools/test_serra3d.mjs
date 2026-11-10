// Prova in Node della card 3D (homeassistant/www/serra3d/serra-3d-card.js),
// senza browser e senza WebGL: costruisce la scena con il vero three.js,
// le applica stati di Home Assistant diversi e anima qualche fotogramma.
//
//     node tools/test_serra3d.mjs
//
// Controlla che la geometria sia sensata (falde del tetto rivolte verso
// l'alto e l'esterno, niente coordinate NaN), che ogni stato -- giorno,
// notte, pioggia, neve, nebbia, entita' mancanti -- si traduca nella scena
// che ci si aspetta, e che l'animazione non produca valori impossibili.

import * as THREE from '../homeassistant/www/serra3d/vendor/three.module.min.js';
import {
  costruisciScena, applicaStato, anima, statoDaHass, coloreTemperatura, ENTITA,
} from '../homeassistant/www/serra3d/serra-3d-card.js';

let errori = 0;
const verifica = (cond, msg) => { if (!cond) { errori++; console.log('FALLITO:', msg); } };

const mondo = costruisciScena();
const { scena, parti } = mondo;

// --- Geometria ---------------------------------------------------------------
let mesh = 0, nan = 0;
scena.updateMatrixWorld(true);
scena.traverse((o) => {
  if (o.isMesh) mesh++;
  const p = new THREE.Vector3();
  o.getWorldPosition(p);
  if (![p.x, p.y, p.z].every(Number.isFinite)) nan++;
});
console.log(`scena: ${mesh} mesh, ${mondo.cliccabili.length} oggetti cliccabili`);
verifica(nan === 0, `${nan} oggetti con posizione non finita`);
verifica(mesh > 50 && mesh < 600, `numero di mesh inatteso: ${mesh}`);

// Falde del tetto: i pannelli di vetro sopra la parete devono avere la
// normale con una componente verso l'alto e verso il proprio lato.
const vetri = [];
scena.traverse((o) => { if (o.isMesh && o.material === parti.vetro && o.geometry.type === 'PlaneGeometry') vetri.push(o); });
verifica(vetri.length === 4, `attesi 4 pannelli (2 pareti, 2 falde), trovati ${vetri.length}`);
for (const v of vetri) {
  const n = new THREE.Vector3(0, 0, 1).applyQuaternion(v.quaternion);
  const lato = Math.sign(v.position.x);
  const bordoAlto = new THREE.Vector3(0, v.geometry.parameters.height / 2, 0).applyQuaternion(v.quaternion).add(v.position);
  const bordoBasso = new THREE.Vector3(0, -v.geometry.parameters.height / 2, 0).applyQuaternion(v.quaternion).add(v.position);
  if (v.position.y > 1.8) {
    // falda: va dalla parete (|x| = 1,3, y = 1,8) al colmo (x = 0, y = 2,6)
    const estremi = [bordoAlto, bordoBasso].sort((a, b) => a.y - b.y);
    verifica(Math.abs(Math.abs(estremi[0].x) - 1.3) < 1e-6 && Math.abs(estremi[0].y - 1.8) < 1e-6,
      `falda ${lato > 0 ? 'destra' : 'sinistra'}: il bordo basso non sta sulla parete (${estremi[0].x.toFixed(2)}, ${estremi[0].y.toFixed(2)})`);
    verifica(Math.abs(estremi[1].x) < 1e-6 && Math.abs(estremi[1].y - 2.6) < 1e-6,
      `falda ${lato > 0 ? 'destra' : 'sinistra'}: il bordo alto non sta sul colmo (${estremi[1].x.toFixed(2)}, ${estremi[1].y.toFixed(2)})`);
    verifica(Math.abs(n.z) < 1e-6, 'falda con la normale lungo la serra');
  } else {
    verifica(Math.abs(Math.abs(v.position.x) - 1.3) < 1e-6 && Math.abs(n.y) < 1e-6, 'parete fuori posto o non verticale');
  }
}

// --- Stati -------------------------------------------------------------------
const ora = Date.parse('2026-11-11T11:00:00Z');
const casa = (meteo, extra = {}) => ({
  states: {
    [ENTITA.temperatura]: { state: '18.4' },
    [ENTITA.umidita]: { state: '72' },
    [ENTITA.vpd]: { state: '0.59' },
    [ENTITA.batteria]: { state: '13.1' },
    [ENTITA.rssi]: { state: '-108' },
    [ENTITA.contatto]: { state: '2026-11-11T10:59:30+00:00' },
    [ENTITA.acquaUltima]: { state: '2.01', last_changed: '2026-11-11T10:59:00+00:00' },
    [ENTITA.acquaOggi]: { state: '2.01' },
    [ENTITA.durata]: { state: '50' },
    [ENTITA.orarioOra]: { state: '12' },
    [ENTITA.orarioMin]: { state: '0' },
    [ENTITA.consigliata]: { state: '30' },
    [ENTITA.previsione]: { state: '18.0' },
    [ENTITA.stato]: { state: 'Tutto ok' },
    [ENTITA.barometrica]: { state: 'Instabile, pioggia piu\' tardi' },
    [ENTITA.sole]: { state: 'above_horizon', attributes: { elevation: 24, azimuth: 175 } },
    [ENTITA.meteo]: { state: meteo, attributes: { cloud_coverage: 80, temperature: 6 } },
    ...extra,
  },
});

let s = statoDaHass(casa('rainy'), ora);
verifica(s.temperatura === 18.4 && s.orario === '12:00', 'lettura dello stato');
verifica(Math.abs(s.daContatto - 30) < 1e-6 && Math.abs(s.daIrrigazione - 60) < 1e-6, 'tempi da contatto e irrigazione');
applicaStato(mondo, s);
verifica(parti.pioggia.visible && !parti.neve.visible, 'pioggia: le gocce non si vedono');
verifica(parti.irriga === true, 'un minuto dopo l\'irrigazione le gocce devono cadere');
verifica(parti.sole.position.z > 0 && parti.sole.position.y > 0, 'a mezzogiorno il sole deve stare a sud (+z) e sopra');
verifica(parti.stelle.material.opacity < 0.05, 'di giorno le stelle si vedono');

applicaStato(mondo, statoDaHass(casa('snowy', {
  [ENTITA.sole]: { state: 'below_horizon', attributes: { elevation: -30, azimuth: 10 } },
  [ENTITA.temperatura]: { state: '-2' },
}), ora));
verifica(parti.neve.visible && !parti.pioggia.visible, 'neve: i fiocchi non si vedono');
verifica(parti.sole.intensity === 0, `di notte il sole non si spegne (${parti.sole.intensity})`);
verifica(parti.vetro.opacity > 0.3, 'sotto zero i vetri devono brinare');
verifica(parti.cime.every((c) => c.visible), 'con la neve le cime devono imbiancarsi');

applicaStato(mondo, statoDaHass(casa('fog'), ora));
verifica(scena.fog.far < 30, 'nebbia: la vista non si accorcia');

// Home Assistant appena avviato: nessuna entita' della serra
const vuoto = statoDaHass({ states: {} }, ora);
applicaStato(mondo, vuoto);
verifica(vuoto.temperatura === null && vuoto.daContatto === Infinity, 'stato vuoto non riconosciuto');
verifica(parti.led && parti.forzaSegnale === 0, 'senza segnale le onde devono sparire');
verifica(coloreTemperatura(null).getHex() === 0x9e9e9e, 'colore di una temperatura sconosciuta');
verifica(coloreTemperatura(-30).getHex() === 0x3f51b5 && coloreTemperatura(80).getHex() === 0xe53935, 'colori agli estremi');

// --- Animazione ----------------------------------------------------------------
applicaStato(mondo, statoDaHass(casa('rainy'), ora));
for (let i = 0; i < 300; i++) anima(mondo, i / 30, 1 / 30);
const pos = parti.pioggia.geometry.attributes.position.array;
verifica(pos.every(Number.isFinite), 'pioggia con coordinate non finite');
let minY = Infinity, maxY = -Infinity;
for (let i = 1; i < pos.length; i += 3) { minY = Math.min(minY, pos[i]); maxY = Math.max(maxY, pos[i]); }
verifica(minY > -1 && maxY < 11, `la pioggia esce dalla scena (y da ${minY.toFixed(1)} a ${maxY.toFixed(1)})`);
verifica(parti.gocce.every((g) => g.visible), 'durante l\'irrigazione le gocce devono vedersi');
verifica(parti.onde.every((o) => o.material.opacity >= 0 && o.material.opacity <= 1), 'opacita\' delle onde fuori scala');

console.log();
if (errori) {
  console.log(`${errori} verifiche fallite.`);
  process.exit(1);
}
console.log('Tutti i casi passano: geometria, giorno e notte, pioggia, neve, nebbia,');
console.log('entita\' mancanti e animazione.');
