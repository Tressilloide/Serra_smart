/*
 * ============================================================================
 *  SERRA SMART — MODELLO 3D INTERATTIVO PER HOME ASSISTANT
 * ============================================================================
 *
 *  Una card personalizzata (type: custom:serra-3d-card) che disegna la serra
 *  di Baselga di Pine' con three.js e la anima con i dati veri:
 *
 *    - il sole sta dove sta davvero (sun.sun: azimut ed elevazione), di
 *      notte il cielo si scurisce e compaiono le stelle;
 *    - pioggia, neve, nebbia e nuvole seguono weather.forecast_home;
 *    - l'aria dentro la serra prende il colore della temperatura, i vetri si
 *      appannano con l'umidita' e brinano sotto 1 °C;
 *    - le onde dell'antenna del nodo sono tante quanto e' forte il segnale
 *      LoRa, il LED lampeggia quando arriva un pacchetto;
 *    - dalla linea a goccia scende l'acqua nei minuti dopo un'irrigazione.
 *
 *  Si gira trascinando, si ingrandisce con la rotella o due dita; un tocco
 *  su un'etichetta o su un oggetto (scatola, batteria, antenna, aiuole,
 *  pannello) apre il dettaglio dell'entita'.
 *
 *  INSTALLAZIONE
 *    /config/www/serra3d/  <- questo file e la cartella vendor/ (three.js r160)
 *    risorsa Lovelace: /local/serra3d/serra-3d-card.js, tipo "module"
 *    card:  type: custom:serra-3d-card   (opzioni: altezza, in pixel)
 *
 *  Il rendering gira solo quando la card e' visibile, a 30 fotogrammi al
 *  secondo al massimo: l'Echo Show e i telefoni ringraziano.
 * ============================================================================
 */

import * as THREE from './vendor/three.module.min.js';
import { OrbitControls } from './vendor/OrbitControls.js';

const VERSIONE = '1.0.0';

export const ENTITA = {
  temperatura: 'sensor.serra_temperatura',
  umidita:     'sensor.serra_umidita_aria',
  vpd:         'sensor.serra_vpd',
  batteria:    'sensor.serra_tensione_batteria',
  rssi:        'sensor.serra_segnale_lora',
  contatto:    'sensor.serra_ultimo_contatto',
  stato:       'sensor.serra_stato_generale',
  acquaOggi:   'sensor.acqua_serra_oggi',
  acquaUltima: 'sensor.serra_acqua_ultima_irrigazione',
  esito:       'sensor.serra_esito_irrigazione',
  durata:      'number.serra_durata_irrigazione',
  consigliata: 'sensor.serra_durata_irrigazione_consigliata',
  previsione:  'sensor.serra_previsione',
  barometrica: 'sensor.serra_previsione_barometrica',
  orarioOra:   'number.serra_orario_irrigazione_ora',
  orarioMin:   'number.serra_orario_irrigazione_minuto',
  sole:        'sun.sun',
  meteo:       'weather.forecast_home',
};

// Misure della serra in metri. Non sono quelle vere al centimetro: servono
// proporzioni credibili, e si cambiano qui.
const SERRA = { larghezza: 2.6, lunghezza: 4.0, parete: 1.8, colmo: 2.6 };

// ---------------------------------------------------------------------------
//  Lettura dello stato da hass, senza toccare la scena (provabile in Node)
// ---------------------------------------------------------------------------

const num = (s) => {
  const v = s ? parseFloat(s.state) : NaN;
  return Number.isFinite(v) ? v : null;
};

export function statoDaHass(hass, adesso = Date.now()) {
  const st = (id) => (hass && hass.states ? hass.states[id] : undefined);
  const sole = st(ENTITA.sole);
  const meteo = st(ENTITA.meteo);
  const contatto = st(ENTITA.contatto);
  const ultima = st(ENTITA.acquaUltima);

  const tContatto = contatto ? Date.parse(contatto.state) : NaN;
  const tIrrig = ultima ? Date.parse(ultima.last_changed) : NaN;

  return {
    temperatura: num(st(ENTITA.temperatura)),
    umidita:     num(st(ENTITA.umidita)),
    vpd:         num(st(ENTITA.vpd)),
    batteria:    num(st(ENTITA.batteria)),
    rssi:        num(st(ENTITA.rssi)),
    acquaOggi:   num(st(ENTITA.acquaOggi)),
    acquaUltima: num(ultima),
    durata:      num(st(ENTITA.durata)),
    consigliata: num(st(ENTITA.consigliata)),
    previsione:  num(st(ENTITA.previsione)),
    orario: (num(st(ENTITA.orarioOra)) !== null)
      ? `${String(num(st(ENTITA.orarioOra))).padStart(2, '0')}:${String(num(st(ENTITA.orarioMin)) ?? 0).padStart(2, '0')}`
      : null,
    stato:       st(ENTITA.stato) ? st(ENTITA.stato).state : null,
    esito:       st(ENTITA.esito) ? st(ENTITA.esito).state : null,
    barometrica: st(ENTITA.barometrica) ? st(ENTITA.barometrica).state : null,
    // Sole: in gradi, azimut da nord in senso orario
    elevazione:  sole && sole.attributes ? Number(sole.attributes.elevation ?? -20) : -20,
    azimut:      sole && sole.attributes ? Number(sole.attributes.azimuth ?? 180) : 180,
    meteo:       meteo ? meteo.state : null,
    nuvole:      meteo && meteo.attributes ? Number(meteo.attributes.cloud_coverage ?? 0) : 0,
    tFuori:      meteo && meteo.attributes ? Number(meteo.attributes.temperature ?? NaN) : NaN,
    // Secondi dall'ultimo pacchetto e dall'ultima irrigazione registrata
    daContatto:  Number.isFinite(tContatto) ? (adesso - tContatto) / 1000 : Infinity,
    daIrrigazione: Number.isFinite(tIrrig) ? (adesso - tIrrig) / 1000 : Infinity,
  };
}

// ---------------------------------------------------------------------------
//  Colori
// ---------------------------------------------------------------------------

// Temperatura -> colore: blu sotto zero, verde fra 15 e 25, rosso oltre 35
export function coloreTemperatura(t) {
  if (t === null || !Number.isFinite(t)) return new THREE.Color(0x9e9e9e);
  const tappe = [[-5, 0x3f51b5], [5, 0x4f8fe6], [15, 0x43a047], [25, 0x9ccc65], [32, 0xfb8c00], [40, 0xe53935]];
  if (t <= tappe[0][0]) return new THREE.Color(tappe[0][1]);
  for (let i = 1; i < tappe.length; i++) {
    if (t <= tappe[i][0]) {
      const k = (t - tappe[i - 1][0]) / (tappe[i][0] - tappe[i - 1][0]);
      return new THREE.Color(tappe[i - 1][1]).lerp(new THREE.Color(tappe[i][1]), k);
    }
  }
  return new THREE.Color(tappe[tappe.length - 1][1]);
}

// Generatore pseudo-casuale con seme: le piante restano uguali a ogni carica
function casuale(seme) {
  let s = seme >>> 0;
  return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296);
}

// ---------------------------------------------------------------------------
//  Costruzione della scena (nessun DOM: si puo' provare in Node)
// ---------------------------------------------------------------------------

export function costruisciScena() {
  const scena = new THREE.Scene();
  const parti = {};
  const cliccabili = [];          // [oggetto, entita]
  const ancore = {};              // punti 3D a cui appendere le etichette
  const rnd = casuale(1000);      // Baselga di Pine', 1000 m

  const mat = (colore, extra = {}) => new THREE.MeshStandardMaterial({ color: colore, roughness: 0.8, ...extra });
  const box = (w, h, d, m) => new THREE.Mesh(new THREE.BoxGeometry(w, h, d), m);
  const ombre = (o) => { o.castShadow = true; o.receiveShadow = true; return o; };

  // --- Luci ------------------------------------------------------------------
  parti.ambiente = new THREE.HemisphereLight(0xcfe8ff, 0x3d5a2a, 0.6);
  scena.add(parti.ambiente);
  parti.sole = new THREE.DirectionalLight(0xfff2d6, 1.6);
  parti.sole.castShadow = true;
  parti.sole.shadow.mapSize.set(1024, 1024);
  const sc = parti.sole.shadow.camera;
  sc.left = -6; sc.right = 6; sc.top = 6; sc.bottom = -6; sc.near = 1; sc.far = 40;
  scena.add(parti.sole, parti.sole.target);
  parti.luna = new THREE.DirectionalLight(0x8ea8ff, 0.0);
  parti.luna.position.set(-8, 10, 6);
  scena.add(parti.luna);

  // --- Terreno, montagne del Pine' e lago della Serraia ------------------------
  const prato = new THREE.Mesh(new THREE.CircleGeometry(70, 48), mat(0x5b8c3a, { roughness: 1 }));
  prato.rotation.x = -Math.PI / 2;
  prato.receiveShadow = true;
  scena.add(prato);
  parti.prato = prato;

  const lago = new THREE.Mesh(new THREE.CircleGeometry(4, 32),
    mat(0x3a6f9a, { roughness: 0.15, metalness: 0.2 }));
  lago.rotation.x = -Math.PI / 2;
  lago.scale.set(1.8, 1, 1);
  lago.position.set(-12, 0.02, -14);
  scena.add(lago);

  parti.cime = [];
  for (let i = 0; i < 9; i++) {
    const a = -Math.PI * 0.95 + i * (Math.PI * 0.9 / 8) + (rnd() - 0.5) * 0.15;
    const r = 44 + rnd() * 10, h = 8 + rnd() * 9;
    const monte = new THREE.Mesh(new THREE.ConeGeometry(9 + rnd() * 6, h, 7), mat(0x4e6b47, { flatShading: true }));
    monte.position.set(Math.cos(a) * r, h / 2 - 0.2, Math.sin(a) * r);
    const neve = new THREE.Mesh(new THREE.ConeGeometry(3.6 + rnd() * 2, h * 0.3, 7), mat(0xf4f7fb, { flatShading: true }));
    neve.position.y = h * 0.36;
    neve.visible = false;              // d'inverno, quando fa freddo fuori
    monte.add(neve);
    parti.cime.push(neve);
    scena.add(monte);
  }

  // Qualche abete attorno
  for (let i = 0; i < 14; i++) {
    const a = rnd() * Math.PI * 2, r = 9 + rnd() * 9;
    const abete = new THREE.Group();
    const tronco = box(0.15, 0.6, 0.15, mat(0x5d4037));
    tronco.position.y = 0.3;
    const chioma = new THREE.Mesh(new THREE.ConeGeometry(0.7, 2.2, 7), mat(0x2e5d34, { flatShading: true }));
    chioma.position.y = 1.6;
    abete.add(tronco, chioma);
    abete.position.set(Math.cos(a) * r, 0, Math.sin(a) * r);
    abete.scale.setScalar(0.8 + rnd() * 0.6);
    scena.add(ombre(abete));
  }

  // --- La serra ------------------------------------------------------------------
  const { larghezza: W, lunghezza: L, parete: H, colmo: R } = SERRA;
  const serra = new THREE.Group();
  scena.add(serra);
  parti.serra = serra;

  const telaio = mat(0xb0bec5, { metalness: 0.6, roughness: 0.35 });
  const vetro = new THREE.MeshStandardMaterial({
    color: 0xe8f4ff, transparent: true, opacity: 0.16, roughness: 0.05, metalness: 0.0,
    side: THREE.DoubleSide, depthWrite: false,
  });
  parti.vetro = vetro;

  const montante = (x1, y1, z1, x2, y2, z2, s = 0.045) => {
    const a = new THREE.Vector3(x1, y1, z1), b = new THREE.Vector3(x2, y2, z2);
    const lung = a.distanceTo(b);
    const m = box(s, lung, s, telaio);
    m.position.copy(a).add(b).multiplyScalar(0.5);
    m.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), b.clone().sub(a).normalize());
    m.castShadow = true;
    serra.add(m);
  };

  // Archi del telaio ogni metro, piu' le travi longitudinali
  const archi = 5;
  for (let i = 0; i < archi; i++) {
    const z = -L / 2 + (L * i) / (archi - 1);
    montante(-W / 2, 0, z, -W / 2, H, z);
    montante(W / 2, 0, z, W / 2, H, z);
    montante(-W / 2, H, z, 0, R, z);
    montante(W / 2, H, z, 0, R, z);
  }
  for (const [x, y] of [[-W / 2, 0.02], [W / 2, 0.02], [-W / 2, H], [W / 2, H], [0, R]])
    montante(x, y, -L / 2, x, y, L / 2);

  // Pannelli laterali e del tetto: un rettangolo lungo tutta la serra (asse
  // z) che va dal bordo (x1, y1) al bordo (x2, y2) della sezione. La base si
  // costruisce a mano: con gli angoli di Eulero le falde del tetto venivano
  // inclinate dalla parte sbagliata.
  const pannello = (x1, y1, x2, y2) => {
    const a = new THREE.Vector3(x1, y1, 0), b = new THREE.Vector3(x2, y2, 0);
    const p = new THREE.Mesh(new THREE.PlaneGeometry(L, a.distanceTo(b)), vetro);
    const asseX = new THREE.Vector3(0, 0, 1);
    const asseY = b.clone().sub(a).normalize();
    const asseZ = new THREE.Vector3().crossVectors(asseX, asseY);
    p.quaternion.setFromRotationMatrix(new THREE.Matrix4().makeBasis(asseX, asseY, asseZ));
    p.position.copy(a).add(b).multiplyScalar(0.5);
    p.renderOrder = 2;
    serra.add(p);
    return p;
  };
  pannello(-W / 2, 0, -W / 2, H);
  pannello(W / 2, 0, W / 2, H);
  pannello(-W / 2, H, 0, R);
  pannello(W / 2, H, 0, R);

  // Timpani a cinque lati, con la porta su quello davanti
  const forma = new THREE.Shape();
  forma.moveTo(-W / 2, 0); forma.lineTo(W / 2, 0); forma.lineTo(W / 2, H);
  forma.lineTo(0, R); forma.lineTo(-W / 2, H); forma.lineTo(-W / 2, 0);
  for (const z of [-L / 2, L / 2]) {
    const t = new THREE.Mesh(new THREE.ShapeGeometry(forma), vetro);
    t.position.z = z;
    t.renderOrder = 2;
    serra.add(t);
  }
  montante(-0.4, 0, L / 2, -0.4, 1.85, L / 2, 0.05);
  montante(0.4, 0, L / 2, 0.4, 1.85, L / 2, 0.05);
  montante(-0.4, 1.85, L / 2, 0.4, 1.85, L / 2, 0.05);
  const maniglia = box(0.03, 0.12, 0.04, mat(0x37474f));
  maniglia.position.set(0.3, 1.0, L / 2 + 0.03);
  serra.add(maniglia);

  // Aria interna: un volume appena colorato dalla temperatura
  parti.aria = new THREE.Mesh(
    new THREE.BoxGeometry(W - 0.1, H - 0.1, L - 0.1),
    new THREE.MeshBasicMaterial({ color: 0x43a047, transparent: true, opacity: 0.07, depthWrite: false }));
  parti.aria.position.y = H / 2;
  parti.aria.renderOrder = 1;
  serra.add(parti.aria);

  // --- Aiuole, piante e linea a goccia -------------------------------------------
  const legno = mat(0x8d6e63);
  const terra = mat(0x4e342e, { roughness: 1 });
  parti.gocciolatori = [];
  for (const x of [-0.65, 0.65]) {
    const aiuola = new THREE.Group();
    const cassa = ombre(box(0.8, 0.35, L - 0.8, legno));
    cassa.position.y = 0.175;
    const suolo = box(0.72, 0.02, L - 0.88, terra);
    suolo.position.y = 0.34;
    suolo.receiveShadow = true;
    aiuola.add(cassa, suolo);

    // Linea a goccia: un tubo nero lungo l'aiuola, con un gocciolatore per pianta
    const tubo = new THREE.Mesh(new THREE.CylinderGeometry(0.012, 0.012, L - 0.9, 8), mat(0x111111));
    tubo.rotation.x = Math.PI / 2;
    tubo.position.set(0.12, 0.37, 0);
    aiuola.add(tubo);

    const piante = 7;
    for (let i = 0; i < piante; i++) {
      const z = -(L - 1.2) / 2 + ((L - 1.2) * i) / (piante - 1);
      const p = new THREE.Group();
      const alt = 0.25 + rnd() * 0.35;
      const stelo = new THREE.Mesh(new THREE.CylinderGeometry(0.012, 0.018, alt, 6), mat(0x558b2f));
      stelo.position.y = alt / 2;
      p.add(stelo);
      const foglie = 4 + Math.floor(rnd() * 3);
      for (let f = 0; f < foglie; f++) {
        const foglia = new THREE.Mesh(new THREE.SphereGeometry(0.07 + rnd() * 0.05, 7, 5),
          mat(new THREE.Color().setHSL(0.27 + rnd() * 0.06, 0.55, 0.32 + rnd() * 0.1), { flatShading: true }));
        foglia.scale.set(1, 0.45, 1.4);
        const a = rnd() * Math.PI * 2;
        foglia.position.set(Math.cos(a) * 0.08, alt * (0.45 + 0.55 * f / foglie), Math.sin(a) * 0.08);
        foglia.rotation.y = a;
        p.add(foglia);
      }
      // Un pomodoro rosso qua e la', per allegria
      if (rnd() < 0.5) {
        const frutto = new THREE.Mesh(new THREE.SphereGeometry(0.035, 8, 6), mat(0xd32f2f, { roughness: 0.4 }));
        frutto.position.set(0.06, alt * 0.55, 0.03);
        p.add(frutto);
      }
      p.position.set((rnd() - 0.5) * 0.1, 0.35, z);
      aiuola.add(ombre(p));
      parti.gocciolatori.push(new THREE.Vector3(x + 0.12, 0.37, z));
    }
    aiuola.position.x = x;
    serra.add(aiuola);
    cliccabili.push([aiuola, ENTITA.acquaUltima]);
  }
  ancore.acqua = new THREE.Vector3(0.65, 0.9, 1.1);

  // Gocce: poche sfere che scendono dai gocciolatori dopo un'irrigazione
  const gocciaGeo = new THREE.SphereGeometry(0.018, 6, 4);
  const gocciaMat = new THREE.MeshStandardMaterial({ color: 0x4fc3f7, roughness: 0.1, transparent: true, opacity: 0.9 });
  parti.gocce = parti.gocciolatori.map((p, i) => {
    const g = new THREE.Mesh(gocciaGeo, gocciaMat);
    g.position.copy(p);
    g.userData.fase = (i * 0.37) % 1;
    g.visible = false;
    serra.add(g);
    return g;
  });

  // Tubo dell'acqua che entra dal fondo, elettrovalvola e flussometro
  const tuboAcqua = new THREE.Mesh(new THREE.CylinderGeometry(0.02, 0.02, 1.0, 8), mat(0x1565c0));
  tuboAcqua.rotation.x = Math.PI / 2;
  tuboAcqua.position.set(0, 0.42, -L / 2 + 0.1);
  serra.add(tuboAcqua);
  parti.valvola = box(0.09, 0.09, 0.07, mat(0x263238));
  parti.valvola.position.set(0, 0.47, -L / 2 + 0.55);
  const bobina = box(0.06, 0.06, 0.06, mat(0xfbc02d));
  bobina.position.y = 0.075;
  parti.valvola.add(bobina);
  serra.add(parti.valvola);
  cliccabili.push([parti.valvola, ENTITA.esito]);

  // --- Il nodo: la scatola di plastica su un palo, con antenna e LED ----------
  const palo = ombre(box(0.05, 1.3, 0.05, mat(0x795548)));
  palo.position.set(0, 0.65, -L / 2 + 0.3);
  serra.add(palo);
  parti.scatola = ombre(box(0.26, 0.18, 0.11, mat(0xf5f5f0, { roughness: 0.5 })));
  parti.scatola.position.set(0, 1.25, -L / 2 + 0.36);
  serra.add(parti.scatola);
  cliccabili.push([parti.scatola, ENTITA.temperatura]);
  ancore.scatola = new THREE.Vector3(0, 1.62, -L / 2 + 0.36);

  parti.led = new THREE.Mesh(new THREE.SphereGeometry(0.014, 8, 6),
    new THREE.MeshBasicMaterial({ color: 0x00e676 }));
  parti.led.position.set(0.09, 1.29, -L / 2 + 0.42);
  serra.add(parti.led);

  const antenna = new THREE.Mesh(new THREE.CylinderGeometry(0.006, 0.006, 0.32, 6), mat(0x212121));
  antenna.position.set(0.1, 1.5, -L / 2 + 0.36);
  serra.add(antenna);
  cliccabili.push([antenna, ENTITA.rssi]);
  parti.onde = [];
  for (let i = 0; i < 3; i++) {
    const onda = new THREE.Mesh(new THREE.TorusGeometry(0.1, 0.006, 6, 24),
      new THREE.MeshBasicMaterial({ color: 0x80deea, transparent: true, opacity: 0.0, depthWrite: false }));
    onda.position.set(0.1, 1.66, -L / 2 + 0.36);
    onda.rotation.x = Math.PI / 2;
    onda.userData.fase = i / 3;
    serra.add(onda);
    parti.onde.push(onda);
  }
  ancore.antenna = new THREE.Vector3(0.1, 1.95, -L / 2 + 0.36);

  // Batteria e regolatore di carica, a terra vicino al palo
  parti.batteria = ombre(box(0.3, 0.2, 0.18, mat(0x263238, { roughness: 0.6 })));
  parti.batteria.position.set(-0.45, 0.1, -L / 2 + 0.3);
  for (const [dx, col] of [[-0.08, 0xd32f2f], [0.08, 0x212121]]) {
    const morsetto = box(0.03, 0.03, 0.03, mat(col));
    morsetto.position.set(dx, 0.115, 0);
    parti.batteria.add(morsetto);
  }
  serra.add(parti.batteria);
  cliccabili.push([parti.batteria, ENTITA.batteria]);
  ancore.batteria = new THREE.Vector3(-0.45, 0.45, -L / 2 + 0.3);
  const regolatore = ombre(box(0.14, 0.1, 0.04, mat(0x1e88e5)));
  regolatore.position.set(-0.1, 0.9, -L / 2 + 0.33);
  serra.add(regolatore);

  // --- Pannello solare fuori, rivolto a sud (+z) -----------------------------
  const solare = new THREE.Group();
  const asta = ombre(box(0.06, 1.4, 0.06, telaio));
  asta.position.y = 0.7;
  const modulo = ombre(box(1.0, 0.03, 0.65, mat(0x0d2a5c, { roughness: 0.25, metalness: 0.5 })));
  modulo.position.y = 1.45;
  modulo.rotation.x = 0.7;
  solare.add(asta, modulo);
  solare.position.set(W / 2 + 0.9, 0, -L / 2 + 0.5);
  scena.add(solare);
  cliccabili.push([solare, ENTITA.batteria]);

  // --- Cielo: stelle, nuvole, pioggia, neve ----------------------------------------
  const stelle = new THREE.BufferGeometry();
  const ps = [];
  for (let i = 0; i < 400; i++) {
    const a = rnd() * Math.PI * 2, e = 0.15 + rnd() * 1.3, r = 90;
    ps.push(Math.cos(a) * Math.cos(e) * r, Math.sin(e) * r, Math.sin(a) * Math.cos(e) * r);
  }
  stelle.setAttribute('position', new THREE.Float32BufferAttribute(ps, 3));
  parti.stelle = new THREE.Points(stelle, new THREE.PointsMaterial({ color: 0xffffff, size: 0.35, transparent: true, opacity: 0 }));
  scena.add(parti.stelle);

  parti.nuvole = [];
  const nuvolaMat = new THREE.MeshStandardMaterial({ color: 0xffffff, roughness: 1, transparent: true, opacity: 0.85, flatShading: true });
  for (let i = 0; i < 7; i++) {
    const n = new THREE.Group();
    for (let j = 0; j < 4; j++) {
      const b = new THREE.Mesh(new THREE.IcosahedronGeometry(0.8 + rnd() * 0.7, 0), nuvolaMat);
      b.position.set(j * 0.9 - 1.3, rnd() * 0.4, rnd() * 0.6);
      n.add(b);
    }
    n.position.set((rnd() - 0.5) * 30, 9 + rnd() * 4, (rnd() - 0.5) * 30);
    n.userData.velocita = 0.15 + rnd() * 0.2;
    scena.add(n);
    parti.nuvole.push(n);
  }
  parti.nuvolaMat = nuvolaMat;

  const N_PIOGGIA = 700;
  const gp = new THREE.BufferGeometry();
  const posP = new Float32Array(N_PIOGGIA * 6);
  for (let i = 0; i < N_PIOGGIA; i++) {
    const x = (rnd() - 0.5) * 16, y = rnd() * 10, z = (rnd() - 0.5) * 16;
    posP.set([x, y, z, x, y + 0.25, z], i * 6);
  }
  gp.setAttribute('position', new THREE.BufferAttribute(posP, 3));
  parti.pioggia = new THREE.LineSegments(gp, new THREE.LineBasicMaterial({ color: 0x9fc3e8, transparent: true, opacity: 0.55 }));
  parti.pioggia.visible = false;
  scena.add(parti.pioggia);

  const N_NEVE = 600;
  const gn = new THREE.BufferGeometry();
  const posN = new Float32Array(N_NEVE * 3);
  for (let i = 0; i < N_NEVE; i++) posN.set([(rnd() - 0.5) * 16, rnd() * 10, (rnd() - 0.5) * 16], i * 3);
  gn.setAttribute('position', new THREE.BufferAttribute(posN, 3));
  parti.neve = new THREE.Points(gn, new THREE.PointsMaterial({ color: 0xffffff, size: 0.08 }));
  parti.neve.visible = false;
  scena.add(parti.neve);

  scena.fog = new THREE.Fog(0xbcd7ef, 30, 110);
  scena.background = new THREE.Color(0x87b8e8);

  return { scena, parti, cliccabili, ancore };
}

// ---------------------------------------------------------------------------
//  Applicazione dello stato alla scena (anche questa senza DOM)
// ---------------------------------------------------------------------------

const PIOGGIA = new Set(['rainy', 'pouring', 'lightning-rainy']);
const NEVE = new Set(['snowy', 'snowy-rainy']);

export function applicaStato(mondo, s) {
  const { scena, parti } = mondo;

  // Sole: da azimut/elevazione a una direzione. Nord = -z, est = +x.
  const el = THREE.MathUtils.degToRad(Math.max(s.elevazione, -10));
  const az = THREE.MathUtils.degToRad(s.azimut);
  const dir = new THREE.Vector3(Math.sin(az) * Math.cos(el), Math.sin(el), -Math.cos(az) * Math.cos(el));
  parti.sole.position.copy(dir.multiplyScalar(20));

  const giorno = THREE.MathUtils.clamp((s.elevazione + 4) / 14, 0, 1);   // 0 notte, 1 giorno pieno
  const coperto = THREE.MathUtils.clamp(s.nuvole / 100, 0, 1);
  parti.sole.intensity = 1.8 * giorno * (1 - 0.65 * coperto);
  parti.ambiente.intensity = 0.15 + 0.55 * giorno;
  parti.luna.intensity = 0.25 * (1 - giorno);

  const cieloGiorno = new THREE.Color(0x87b8e8).lerp(new THREE.Color(0xa9b4bf), coperto);
  const cieloNotte = new THREE.Color(0x0b1430);
  const tramonto = new THREE.Color(0xf4a46a);
  const cielo = cieloNotte.clone().lerp(cieloGiorno, giorno);
  if (giorno > 0 && giorno < 0.6) cielo.lerp(tramonto, 0.35 * (1 - Math.abs(giorno - 0.3) / 0.3));
  scena.background.copy(cielo);
  scena.fog.color.copy(cielo);
  parti.stelle.material.opacity = (1 - giorno) * (1 - coperto) * 0.9;

  // Meteo
  const meteo = s.meteo || '';
  parti.pioggia.visible = PIOGGIA.has(meteo);
  parti.neve.visible = NEVE.has(meteo);
  const nebbia = meteo === 'fog';
  scena.fog.near = nebbia ? 3 : 30;
  scena.fog.far = nebbia ? 22 : 110;
  const quanteNuvole = Math.round(parti.nuvole.length * coperto);
  parti.nuvole.forEach((n, i) => { n.visible = i < quanteNuvole; });
  parti.nuvolaMat.color.set(PIOGGIA.has(meteo) || meteo === 'lightning' ? 0x8a929b : 0xffffff);
  const freddoFuori = Number.isFinite(s.tFuori) ? s.tFuori < 3 : false;
  parti.cime.forEach((c) => { c.visible = freddoFuori || NEVE.has(meteo); });
  parti.prato.material.color.set(NEVE.has(meteo) ? 0xe8eef3 : 0x5b8c3a);

  // Aria interna e vetri
  parti.aria.material.color.copy(coloreTemperatura(s.temperatura));
  parti.aria.material.opacity = s.temperatura === null ? 0.03 : 0.08;
  const condensa = s.umidita === null ? 0 : THREE.MathUtils.clamp((s.umidita - 70) / 30, 0, 1);
  const brina = s.temperatura !== null && s.temperatura < 1;
  parti.vetro.opacity = 0.14 + 0.3 * condensa + (brina ? 0.2 : 0);
  parti.vetro.color.set(brina ? 0xf3fbff : 0xe8f4ff);

  // Batteria: rossa sotto 11,5 V, gialla sotto 12,2
  const v = s.batteria;
  parti.batteria.material.color.set(v === null ? 0x455a64 : v < 11.5 ? 0xb71c1c : v < 12.2 ? 0xf9a825 : 0x263238);

  // Segnale: da -125 (niente) a -80 dBm (pieno), per le onde dell'antenna
  parti.forzaSegnale = s.rssi === null ? 0 : THREE.MathUtils.clamp((s.rssi + 125) / 45, 0.08, 1);
  parti.irriga = s.daIrrigazione < 180;
  parti.ultimoContatto = s.daContatto;
}

// Animazione: t in secondi, dt dall'ultimo fotogramma
export function anima(mondo, t, dt) {
  const { parti } = mondo;

  parti.onde.forEach((o) => {
    const f = (t * 0.6 + o.userData.fase) % 1;
    o.scale.setScalar(1 + f * 3);
    o.material.opacity = (1 - f) * 0.7 * parti.forzaSegnale;
  });

  // LED: lampeggio veloce nel primo minuto dopo un pacchetto, poi un battito
  // lento; spento (rosso) se il nodo tace da oltre mezz'ora
  const muto = parti.ultimoContatto > 1800;
  const fresco = parti.ultimoContatto < 60;
  parti.led.material.color.set(muto ? 0xd50000 : 0x00e676);
  parti.led.visible = muto || (fresco ? (t * 4) % 1 < 0.5 : (t % 3) < 0.15);

  parti.gocce.forEach((g, i) => {
    g.visible = parti.irriga;
    if (!parti.irriga) return;
    const f = (t * 1.2 + g.userData.fase) % 1;
    const p = parti.gocciolatori[i];
    g.position.set(p.x, p.y - f * 0.03, p.z);
    g.scale.setScalar(0.6 + f * 0.6);
  });

  const cade = (geo, step, velocita, piano = 0) => {
    const a = geo.attributes.position;
    for (let i = 0; i < a.count; i += step) {
      let y = a.getY(i) - velocita * dt;
      if (y < piano) y += 10;
      for (let k = 0; k < step; k++) a.setY(i + k, y + (k ? 0.25 : 0));
    }
    a.needsUpdate = true;
  };
  if (parti.pioggia.visible) cade(parti.pioggia.geometry, 2, 9);
  if (parti.neve.visible) cade(parti.neve.geometry, 1, 0.8);

  parti.nuvole.forEach((n) => {
    n.position.x += n.userData.velocita * dt;
    if (n.position.x > 18) n.position.x = -18;
  });
}

// ---------------------------------------------------------------------------
//  La card
// ---------------------------------------------------------------------------

const fmt = (v, dec, unita) => (v === null || !Number.isFinite(v) ? '—' : `${v.toFixed(dec)}${unita}`);

// In Node (tools/test_serra3d.mjs) HTMLElement non esiste: la classe nasce
// comunque, ma non viene registrata.
const Base = typeof HTMLElement !== 'undefined' ? HTMLElement : class {};

class Serra3dCard extends Base {
  setConfig(config) {
    this._config = { altezza: 420, ...config };
    if (this._radice) this._radice.style.height = `${this._config.altezza}px`;
  }

  static getStubConfig() { return {}; }

  getCardSize() { return Math.ceil((this._config?.altezza ?? 420) / 50); }

  getGridOptions() { return { columns: 12, rows: 8, min_columns: 6, min_rows: 5 }; }

  set hass(hass) {
    this._hass = hass;
    if (!this._mondo) return;
    // Al massimo una volta al secondo: Home Assistant chiama questo setter a
    // ogni cambio di QUALUNQUE entita' della casa. L'ultimo aggiornamento
    // rimandato non si perde: parte allo scadere del secondo.
    const ora = Date.now();
    if (this._ultimoStato && ora - this._ultimoStato < 1000) {
      if (!this._rimandato) this._rimandato = setTimeout(() => {
        this._rimandato = null;
        this._ultimoStato = 0;
        this.hass = this._hass;
      }, 1000 - (ora - this._ultimoStato));
      return;
    }
    this._ultimoStato = ora;
    this._stato = statoDaHass(hass, ora);
    applicaStato(this._mondo, this._stato);
    this._aggiornaEtichette();
  }

  connectedCallback() {
    if (!this._radice) this._crea();
    this._avvia();
  }

  disconnectedCallback() {
    this._ferma();
  }

  _crea() {
    const ombra = this.attachShadow({ mode: 'open' });
    ombra.innerHTML = `
      <style>
        :host { display: block; }
        ha-card { overflow: hidden; position: relative; }
        .radice { position: relative; width: 100%; touch-action: none; }
        canvas { display: block; width: 100%; height: 100%; outline: none; }
        .etichetta {
          position: absolute; transform: translate(-50%, -100%); white-space: nowrap;
          padding: 3px 8px; border-radius: 10px; font: 600 12px/1.3 var(--paper-font-body1_-_font-family, sans-serif);
          background: rgba(20, 28, 36, 0.72); color: #fff; cursor: pointer; pointer-events: auto;
          box-shadow: 0 1px 4px rgba(0,0,0,.35); user-select: none;
        }
        .etichetta::after {
          content: ''; position: absolute; left: 50%; bottom: -5px; margin-left: -5px;
          border: 5px solid transparent; border-bottom: 0; border-top-color: rgba(20, 28, 36, 0.72);
        }
        .etichetta.nascosta { display: none; }
        .testata, .piede {
          position: absolute; left: 0; right: 0; padding: 8px 12px; pointer-events: none;
          font: 13px/1.35 var(--paper-font-body1_-_font-family, sans-serif); color: #fff;
          text-shadow: 0 1px 3px rgba(0,0,0,.7);
        }
        .testata { top: 0; display: flex; justify-content: space-between; gap: 8px; }
        .piede { bottom: 0; background: linear-gradient(transparent, rgba(0,0,0,.45)); }
        .titolo { font-weight: 700; font-size: 15px; }
        .riga { display: flex; flex-wrap: wrap; gap: 4px 14px; }
        .aiuto { position: absolute; right: 10px; bottom: 44px; font-size: 11px; opacity: .75; color: #fff;
                 text-shadow: 0 1px 2px rgba(0,0,0,.7); pointer-events: none; }
        .errore { padding: 16px; color: var(--error-color, #c62828); }
      </style>
      <ha-card>
        <div class="radice">
          <div class="testata">
            <div><div class="titolo">Serra · Baselga di Piné</div><div class="sottotitolo"></div></div>
            <div class="stato"></div>
          </div>
          <div class="piede"><div class="riga"></div></div>
          <div class="aiuto">trascina per girare · tocca per i dettagli</div>
        </div>
      </ha-card>`;
    this._radice = ombra.querySelector('.radice');
    this._radice.style.height = `${this._config?.altezza ?? 420}px`;

    try {
      this._renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: 'low-power' });
    } catch (e) {
      this._radice.innerHTML = '<div class="errore">Il browser non supporta WebGL: il modello 3D non si puo\' mostrare.</div>';
      return;
    }
    this._renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    this._renderer.shadowMap.enabled = true;
    this._renderer.shadowMap.type = THREE.PCFSoftShadowMap;
    this._renderer.outputColorSpace = THREE.SRGBColorSpace;
    this._radice.prepend(this._renderer.domElement);

    this._mondo = costruisciScena();
    this._camera = new THREE.PerspectiveCamera(45, 1, 0.1, 250);
    this._camera.position.set(5.5, 3.6, 6.5);

    this._controlli = new OrbitControls(this._camera, this._renderer.domElement);
    this._controlli.target.set(0, 1.0, 0);
    this._controlli.enableDamping = true;
    this._controlli.minDistance = 2.5;
    this._controlli.maxDistance = 22;
    this._controlli.maxPolarAngle = Math.PI / 2 - 0.05;    // mai sotto terra
    this._controlli.autoRotate = true;
    this._controlli.autoRotateSpeed = 0.5;
    this._controlli.addEventListener('start', () => {
      this._controlli.autoRotate = false;
      clearTimeout(this._timerRotazione);
    });
    this._controlli.addEventListener('end', () => {
      this._timerRotazione = setTimeout(() => { this._controlli.autoRotate = true; }, 20000);
    });

    // Etichette HTML agganciate a punti 3D
    const defs = [
      ['scatola', ENTITA.temperatura, (s) => `🌡️ ${fmt(s.temperatura, 1, ' °C')} · 💧 ${fmt(s.umidita, 0, ' %')}`],
      ['antenna', ENTITA.rssi, (s) => `📶 ${fmt(s.rssi, 0, ' dBm')}`],
      ['batteria', ENTITA.batteria, (s) => `🔋 ${fmt(s.batteria, 2, ' V')}`],
      ['acqua', ENTITA.acquaOggi, (s) => `🚿 oggi ${fmt(s.acquaOggi, 2, ' L')}`],
    ];
    this._etichette = defs.map(([ancora, entita, testo]) => {
      const el = document.createElement('div');
      el.className = 'etichetta nascosta';
      el.addEventListener('click', (ev) => { ev.stopPropagation(); this._dettaglio(entita); });
      this._radice.appendChild(el);
      return { ancora: this._mondo.ancore[ancora], entita, testo, el };
    });

    // Tocco su un oggetto: raggio dalla camera, solo se non si stava girando
    this._raggio = new THREE.Raycaster();
    let giu = null;
    this._renderer.domElement.addEventListener('pointerdown', (e) => { giu = [e.clientX, e.clientY]; });
    this._renderer.domElement.addEventListener('pointerup', (e) => {
      if (!giu || Math.hypot(e.clientX - giu[0], e.clientY - giu[1]) > 6) return;
      const r = this._renderer.domElement.getBoundingClientRect();
      const p = new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
      this._raggio.setFromCamera(p, this._camera);
      const oggetti = this._mondo.cliccabili.map(([o]) => o);
      const colpito = this._raggio.intersectObjects(oggetti, true)[0];
      if (!colpito) return;
      for (const [o, entita] of this._mondo.cliccabili) {
        let x = colpito.object;
        while (x) { if (x === o) return this._dettaglio(entita); x = x.parent; }
      }
    });

    this._ridimensiona = new ResizeObserver(() => this._adatta());
    this._ridimensiona.observe(this._radice);
    this._visibilita = new IntersectionObserver((v) => {
      this._visibile = v.some((x) => x.isIntersecting);
      if (this._visibile) this._avvia();
    });
    this._visibilita.observe(this._radice);

    if (this._hass) { const h = this._hass; this._hass = null; this.hass = h; }
  }

  _dettaglio(entityId) {
    this.dispatchEvent(new CustomEvent('hass-more-info', { detail: { entityId }, bubbles: true, composed: true }));
  }

  _adatta() {
    if (!this._renderer) return;
    const w = this._radice.clientWidth, h = this._radice.clientHeight;
    if (!w || !h) return;
    this._renderer.setSize(w, h, false);
    this._camera.aspect = w / h;
    this._camera.updateProjectionMatrix();
  }

  _aggiornaEtichette() {
    const s = this._stato;
    if (!s || !this._etichette) return;
    for (const e of this._etichette) e.el.textContent = e.testo(s);

    this._radice.querySelector('.stato').textContent = s.stato ?? '';
    const sotto = [];
    if (Number.isFinite(s.tFuori)) sotto.push(`fuori ${s.tFuori.toFixed(0)} °C`);
    if (s.barometrica) sotto.push(s.barometrica);
    this._radice.querySelector('.sottotitolo').textContent = sotto.join(' · ');

    const riga = [];
    if (s.orario) riga.push(`⏰ irrigazione ${s.orario} per ${fmt(s.durata, 0, ' s')}`);
    if (s.consigliata !== null) riga.push(`🌱 consigliata ${fmt(s.consigliata, 0, ' s')}`);
    if (s.previsione !== null) riga.push(`☀️ massima prevista in serra ${fmt(s.previsione, 0, ' °C')}`);
    if (s.vpd !== null) riga.push(`VPD ${fmt(s.vpd, 2, ' kPa')}`);
    this._radice.querySelector('.piede .riga').textContent = riga.join('   ');
  }

  _posizionaEtichette() {
    const w = this._radice.clientWidth, h = this._radice.clientHeight;
    const v = new THREE.Vector3();
    for (const e of this._etichette) {
      v.copy(e.ancora).project(this._camera);
      const fuori = v.z > 1 || Math.abs(v.x) > 1.05 || Math.abs(v.y) > 1.05;
      e.el.classList.toggle('nascosta', fuori || !this._stato);
      if (!fuori) {
        e.el.style.left = `${((v.x + 1) / 2) * w}px`;
        e.el.style.top = `${((1 - v.y) / 2) * h}px`;
      }
    }
  }

  _avvia() {
    if (!this._renderer || this._ciclo) return;
    const orologio = new THREE.Clock();
    let accumulo = 0;
    const passo = () => {
      if (!this.isConnected || this._visibile === false || document.hidden) { this._ciclo = null; return; }
      this._ciclo = requestAnimationFrame(passo);
      const dt = Math.min(orologio.getDelta(), 0.1);
      accumulo += dt;
      if (accumulo < 1 / 30) return;            // 30 fotogrammi al secondo bastano
      const t = orologio.elapsedTime;
      if (this._stato) {
        // ogni tanto si ricalcolano anche le cose che dipendono dal tempo
        // (LED, gocce) pur senza nuovi stati da Home Assistant
        this._stato.daContatto += accumulo;
        this._stato.daIrrigazione += accumulo;
        this._mondo.parti.ultimoContatto = this._stato.daContatto;
        this._mondo.parti.irriga = this._stato.daIrrigazione < 180;
      }
      anima(this._mondo, t, accumulo);
      accumulo = 0;
      this._controlli.update();
      this._renderer.render(this._mondo.scena, this._camera);
      this._posizionaEtichette();
    };
    if (!this._sulVisibile) {
      this._sulVisibile = () => { if (!document.hidden) this._avvia(); };
      document.addEventListener('visibilitychange', this._sulVisibile);
    }
    this._adatta();
    this._ciclo = requestAnimationFrame(passo);
  }

  _ferma() {
    if (this._ciclo) cancelAnimationFrame(this._ciclo);
    this._ciclo = null;
  }
}

if (typeof customElements !== 'undefined' && !customElements.get('serra-3d-card')) {
  customElements.define('serra-3d-card', Serra3dCard);
  window.customCards = window.customCards || [];
  window.customCards.push({
    type: 'serra-3d-card',
    name: 'Serra 3D',
    description: 'Modello 3D interattivo della serra, animato con i dati veri.',
    preview: false,
  });
  console.info(`%c SERRA-3D-CARD %c ${VERSIONE} `, 'background:#2e7d32;color:#fff', 'background:#eee;color:#2e7d32');
}
