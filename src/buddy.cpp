// Buddy registry + lifecycle for the AMOLED port. A clean reimplementation
// of the buddy.h API that reuses the 18 species translation units verbatim
// — they only depend on the buddy_common.h helpers (buddyPrintLine,
// buddySetCursor, …) which main.cpp defines for the Surface backing. The
// buddy is always drawn full-screen at SCALE=4 into the shared Surface.

#include "buddy.h"
#include "buddy_common.h"
#include "stats.h"
#include <Arduino.h>
#include <string.h>

extern const Species CAPYBARA_SPECIES;
extern const Species DUCK_SPECIES;
extern const Species GOOSE_SPECIES;
extern const Species BLOB_SPECIES;
extern const Species CAT_SPECIES;
extern const Species DRAGON_SPECIES;
extern const Species OCTOPUS_SPECIES;
extern const Species OWL_SPECIES;
extern const Species PENGUIN_SPECIES;
extern const Species TURTLE_SPECIES;
extern const Species SNAIL_SPECIES;
extern const Species GHOST_SPECIES;
extern const Species AXOLOTL_SPECIES;
extern const Species CACTUS_SPECIES;
extern const Species ROBOT_SPECIES;
extern const Species RABBIT_SPECIES;
extern const Species MUSHROOM_SPECIES;
extern const Species CHONK_SPECIES;
extern const Species EMBER_SPECIES;

static const Species* SPECIES_TABLE[] = {
  &CAPYBARA_SPECIES, &DUCK_SPECIES,    &GOOSE_SPECIES,   &BLOB_SPECIES,
  &CAT_SPECIES,      &DRAGON_SPECIES,  &OCTOPUS_SPECIES, &OWL_SPECIES,
  &PENGUIN_SPECIES,  &TURTLE_SPECIES,  &SNAIL_SPECIES,   &GHOST_SPECIES,
  &AXOLOTL_SPECIES,  &CACTUS_SPECIES,  &ROBOT_SPECIES,   &RABBIT_SPECIES,
  &MUSHROOM_SPECIES, &CHONK_SPECIES,  &EMBER_SPECIES,
};
static const uint8_t N_SPECIES = sizeof(SPECIES_TABLE) / sizeof(SPECIES_TABLE[0]);
static uint8_t  currentIdx  = 0;
static uint32_t tickCount   = 0;
static uint32_t nextTickAt  = 0;
static const uint32_t TICK_MS = 200;
void buddyInit() {
  uint8_t saved = speciesIdxLoad();
  // 0xFF is the "boot into GIF" sentinel elsewhere; here it just means "no
  // saved choice yet", so fall through to default 0.
  if (saved < N_SPECIES) currentIdx = saved;
  tickCount = 0;
  nextTickAt = 0;
}

void buddySetSpeciesIdx(uint8_t idx) {
  if (idx < N_SPECIES) currentIdx = idx;
}

uint8_t buddySpeciesIdx()      { return currentIdx; }
uint8_t buddySpeciesCount()    { return N_SPECIES; }

void buddyTick(uint8_t personaState) {
  uint32_t now = millis();
  if ((int32_t)(now - nextTickAt) >= 0) {
    nextTickAt = now + TICK_MS;
    tickCount++;
  }
  if (personaState >= 7) personaState = 1;   // safety: fall back to IDLE
  const Species* sp = SPECIES_TABLE[currentIdx];
  if (sp->states[personaState]) sp->states[personaState](tickCount);
}
