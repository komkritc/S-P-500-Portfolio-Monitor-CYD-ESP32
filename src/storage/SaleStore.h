// SaleStore.h - LittleFS side of the sale ledger: append, journal, recovery, history, CSV.
//
//   /sells.jsonl   append-only, one JSON object per line (see SaleEngine.h for the record format)
//   /txn.pending   in-flight transaction: written BEFORE the append, removed AFTER the portfolio
//                  has been saved. A power cut anywhere in between is repaired at the next boot.
//
// Commit order (never reordered):
//   1. write /txn.pending          (atomic: .tmp + rename)
//   2. append line + '\n'          to /sells.jsonl
//   3. apply to the open positions in RAM
//   4. save /portfolio.json        (atomic; carries appliedSeq/ver/nextTxn)
//   5. remove /txn.pending
#pragma once
#include <Arduino.h>
#include "../core/AppState.h"
#include "../core/JsonWriter.h"

namespace storage {

// Replays /sells.jsonl into the open positions and the realised summary, repairing a partial
// tail and completing an in-flight transaction first. Call once at boot, after loadPortfolio().
bool ledgerBoot(AppState &a);

// Preview only (no flash writes). `req.price` must already be the execution price.
bool salePreview(AppState &a, const SaleRequest &req, SalePreview &pv);

// Confirm + commit. `req.token` must match the preview token of the current portfolio/ledger.
// `code` (optional) receives the HTTP status the caller should use: 400 bad input, 409 conflict
// (stale preview / already reversed / ledger full), 500 storage failure.
bool saleCommit(AppState &a, const SaleRequest &req, SalePreview &pv, char *err, size_t errN, int *code = nullptr);

// Appends the reversal of the sale with `seq` (0 = the newest sale).
bool saleReverse(AppState &a, uint32_t seq, char *err, size_t errN, int *code = nullptr);

// Permanently removes one transaction from the history. A sale and its reversal always go together
// (`seq` may name either). A sale that is still active is reversed first, so the position is
// restored before anything is removed. Remaining records are renumbered (ids TX-xxxxxx are kept).
bool saleDelete(AppState &a, uint32_t seq, char *err, size_t errN, int *code = nullptr);

// Newest-first page of history (`total` = records matching `symbol`, "" = all).
// Rows carry their status: REC_SALE / REC_REVERSAL / SALE_ST_REVERSED.
struct SalePage {
  uint16_t total;
  uint16_t count;
  SaleLite rows[SALES_PAGE_MAX];
};
bool salePage(const AppState &a, const char *symbol, uint16_t offset, uint16_t limit, SalePage &out, char *err, size_t errN);

// true when a reversal of `seq` already exists.
bool saleReversed(const AppState &a, uint32_t seq, bool &out);

// Lot inspector (read-only, never writes flash). Streams one JSON object listing every open lot
// of `symbol` with its net sold quantity, how many sale records touched it, and up to 12 most
// recent ledger references (seq, TX id, date, type, qty moved, before -> after, sale-level
// realised). `ledger:false` means the ledger file could not be opened: the lot list is still
// correct but every history field reads as if nothing had been sold.
bool lotsJson(const AppState &a, const char *symbol, JsonWriter &w, char *err, size_t errN);

// Streams matching records as CSV (header + one line per record) through `sink`.
typedef void (*CsvSink)(void *ctx, const char *data, size_t len);
bool saleCsv(const AppState &a, const char *symbol, CsvSink sink, void *ctx, char *err, size_t errN);
}
