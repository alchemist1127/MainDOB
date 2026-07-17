# DobWrite — word processor per MainDOB

Elaboratore di testi a impaginazione (stile Office) per MainDOB, con font
TTF/OTF reali, modello documento, layout e rendering propri. Albero **già
integrato e cablato**: basta `make`.

## Architettura (oggetti-foglio, incrementale, RAM)

Il modello è quello dei word processor a flusso: sorgente continua ->
fogli materializzati, e si renderizza solo il visibile.

- **`libdobfont`** — motore font device-independent (TTF/OTF, glyf+CFF/CID),
  rasterizza glifi in copertura R8.
- **`libdobdoc`** — modello documento: testo (piece table), run di
  attributi carattere/paragrafo (grassetto, corsivo, **sottolineato,
  barrato**, dimensione, colore), stili, undo/redo, file `.dobw`.
- **`libdoblayout`** — layout **persistente, incrementale, interrogabile**:
  impagina in una colonna continua in content-space; un edit re-impagina
  solo il paragrafo toccato (gli altri traslano); query byte<->geometria
  per caret e click.
- **`libdobpage`** — **engine a oggetti-foglio**: pagina la colonna in
  fogli, ognuno con una **superficie in RAM** da un pool (solo i fogli
  visibili sono vivi, memoria O(pagine visibili)), e compone il contenuto
  — carta, margini, testo, colori, righe di sottolineato/barrato — **dentro
  la superficie del foglio**. Aggiornamento **per dirty-rect**: digitare
  una lettera ridisegna solo la striscia toccata; i glifi si rasterizzano
  una volta e restano in cache. Niente GPU: la scheda fa solo il blit.
- **`DobWrite`** — l'app: finestra con **ribbon** nativa che guida
  l'engine; caret e selezione come overlay.

## L'app

Font **integrato**: Times New Roman embeddato nel binario (`font_times.c`).
La **ribbon** è costruita coi widget di MainDOB: `dropdown` Font e
Dimensione, e `picturebutton` toggle per Grassetto/Corsivo/Sottolineato/
Barrato (le icone B/I/U/S sono rese dal motore font). Sotto, l'area fogli:
l'app blitta i fogli visibili (`dp_visible_pages`), risolve i click in
posizione nel testo (`dp_window_to_content` -> `df_layout_hit`) e disegna il
caret (`df_layout_locate` -> `dp_content_to_window`); la selezione è una
tinta su una copia di lavoro del foglio. Ogni modifica chiama
`df_layout_reflow` + `dp_notify_edit`, così si re-impagina solo il paragrafo
e si ridipinge solo la striscia. Apri/salva `.dobw`.

Comandi: dropdown Font/Dimensione; toggle B/I/U/S; digitazione, Invio,
Backspace/Canc, frecce, Inizio/Fine, PgSu/Giù; selezione con Shift o
trascinamento; Ctrl+B/I/U, Ctrl+Z/Y, Ctrl+A, Ctrl+S/O; rotellina per
scorrere.

## Validatori headless

```
fonttest /font.otf A 64     # motore font: rasterizza un glifo (OTF/CFF)
doctest                      # modello: round-trip .dobw in memoria
laytest  /font.ttf           # layout: prova l'incrementalità (reflow di 1 paragrafo)
pagetest /font.ttf           # engine-foglio: paginazione, pool, miniatura del foglio
```

## Stato

Stack completo end-to-end con l'architettura corretta: font -> modello ->
layout incrementale -> engine a oggetti-foglio -> app con ribbon. Solo i
fogli visibili sono in RAM; gli edit sono incrementali (paragrafo + dirty
strip).

Semplificazioni v1: un solo font integrato; incrementalità a livello di
paragrafo (un edit re-shapa il paragrafo intero, non la singola riga);
ripaginazione O(righe) ad ogni edit; input tastiera ASCII; i toggle di
formato agiscono sulla selezione (niente "formato in sospeso" senza
selezione); nessun controllo di verso d'inserimento (bidi). Sviluppi:
incrementalità sub-paragrafo, più font via `fontd`, input UTF-8, GPOS/GSUB/
kerning.
