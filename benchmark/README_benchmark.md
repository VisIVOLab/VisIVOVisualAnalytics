# Benchmark VisIVO Next — protocollo per la Tabella 2 del paper

Due script, zero dipendenze esotiche (`requests`, `numpy`, `astropy`).
Obiettivo: riempire la Tabella 2 con numeri riproducibili e pubblicabili
("idealmente con script pubblici" — questi possono finire nel repo, cartella
`benchmarks/`, e citati nel paper nella sezione Data & software availability).

## 1. Preparazione

Backend avviato sull'host da misurare (embedded sul Mac, o remoto/HPC):

    cd backend && .venv/bin/uvicorn app.main:app --port 8000

Il token è in `~/.visivo_token` (lo script lo legge da solo con backend
locale; con backend remoto passalo con `--token`).

### Cubi di test

**Opzione A — sintetici (riproducibili al bit, consigliata per la Tabella 2):**

    python3 make_synthetic_cube.py --gb 1  --out /data/cube_1gb.fits
    python3 make_synthetic_cube.py --gb 8  --out /data/cube_8gb.fits
    python3 make_synthetic_cube.py --gb 30 --out /data/cube_30gb.fits

Ogni cubo contiene rumore gaussiano + un disco rotante iniettato (PA 30°,
inc 60°, S/N di picco ~12) centrato nel cubo: il seed automatico del
benchmark (pixel più brillante del canale centrale) cade sul disco per
costruzione, e il lasso ha un oggetto cinematicamente esteso realistico.

**Opzione B — dati reali (per il testo degli use case):** un campo WALLABY
pubblico. Passa `--seed Z,Y,X` sulla galassia scelta.

## 2. Esecuzione

    python3 benchmark_visivo.py \
        --cube /data/cube_1gb.fits \
        --cube /data/cube_8gb.fits \
        --cube /data/cube_30gb.fits \
        --repeats 3 --out results_$(hostname)

Note:
- I percorsi `--cube` sono percorsi SUL FILESYSTEM DEL BACKEND.
- Ogni ripetizione varia leggermente i parametri (reach del lasso, canale
  iniziale di M0, vertici della PV) per impedire alla cache LRU dei prodotti
  di falsare i tempi. Riportati mediana ± 1.4826·MAD.
- Sul cubo da 30 GB il full-res di default non viene misurato (`(ROI)` in
  tabella): sopra `--roi-only-above 10` GB si misura solo il camera-ROI
  (half-box centrale, 1/8 dei voxel), coerente con la raccomandazione d'uso
  del tool. Per misurare comunque il full-res: `--roi-only-above 100`.
- Prima misura "a freddo" vs ripetizioni "a caldo" (page cache del SO):
  per la tabella usare le mediane; se vuoi anche il cold-start, riavvia il
  backend e svuota la page cache (macOS: `sudo purge`) tra i run e usa
  `--repeats 1` in un run separato.
- COMPLETARE in `results.json` → `environment.note` la spec hardware
  (CPU, RAM, GPU, disco, filesystem): va nella didascalia della Tabella 2.

## 3. Output

- `results.md` — tabella leggibile;
- `table2_rows.tex` — righe LaTeX pronte da incollare nella Tabella 2
  (stessa etichettatura delle righe del paper);
- `results.json` — dati grezzi + record d'ambiente (da archiviare, ed
  eventualmente da depositare su Zenodo insieme agli script).

## 4. Colonna CARTA (contestualizzazione, non gara)

Stesso host, stesso cubo da 8 GB (`cube_8gb.fits`), CARTA più recente.
Protocollo manuale (3 ripetizioni, riportare la mediana):

| riga                | cosa misurare in CARTA                                        |
|---------------------|---------------------------------------------------------------|
| Preview interactive | File → Open → tempo fino al primo render interattivo del canale |
| Full resolution     | tempo fino a immagine full-res del canale corrente (progress bar) |
| Lasso grow          | **n/a** (nessuna operazione equivalente)                       |
| Masked M0           | Moment Generator: intervallo completo, maschera 3σ → tempo di generazione |
| PV extraction       | PV Generator: linea a 3 segmenti equivalente, width 3 → tempo  |

Se hai il backend CARTA con `--enable_scripting`, le stesse misure si
possono automatizzare col pacchetto Python `carta`; per la tabella la
misura manuale con cronometro (±0.5 s) è comunque adeguata, dichiarandola
in didascalia. Nota metodologica già nel paper: "contextualizes rather
than ranks: the two tools optimize different interactions" — CARTA fa
rendering server-side di tile 2D, VisIVO trasferisce i voxel per il volume
rendering 3D locale: le due colonne misurano operazioni omologhe, non
identiche. Dichiararlo in didascalia (già fatto).

## 5. Integrazione nel paper

1. Incolla `table2_rows.tex` nella Tabella 2 (le righe combaciano).
2. Didascalia: hardware, versione VisIVO Next (commit), versione CARTA,
   dataset sintetici con seed RNG 42 (riproducibili), mediana di 3 run.
3. `benchmark_visivo.py` + `make_synthetic_cube.py` nel repo pubblico →
   citarli in "Data and software availability" ("benchmark scripts and
   synthetic datasets are available at ...").
4. Il numero "lasso grow" sul cubo reale WALLABY (con `--seed` sulla
   galassia) è anche il numero da citare nel testo dell'use case HI.
