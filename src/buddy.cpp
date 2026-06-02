// Buddy registry + lifecycle for the AMOLED port. M5's buddy.cpp pulls in
// M5StickCPlus.h and TFT_eSprite which the new build doesn't have; rather
// than #ifdef-ing that file, this is a clean reimplementation of the same
// buddy.h API. It reuses the 18 species translation units verbatim — they
// only depend on the buddy_common.h helpers (buddyPrintLine, buddySetCursor,
// …) which main.cpp defines for the Surface backing.
//
// Differences from the M5 version:
//   • No render-target indirection. Stage3j owns the Surface, and the
//     buddy_common helpers always draw into it.
//   • No peek scale toggle. The AMOLED draws all species at SCALE=4
//     full-screen — peek mode existed for the M5's tight 135×240 layout
//     where PET/INFO views had to share the panel with the buddy.
//   • buddyRenderTo() is a stub. M5 used it for the landscape clock face
//     (a direct-to-LCD draw at rotation 1/3); AMOLED's clock is portrait
//     only for now, see backlog.

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

static const Species* SPECIES_TABLE[] = {
  &CAPYBARA_SPECIES, &DUCK_SPECIES,    &GOOSE_SPECIES,   &BLOB_SPECIES,
  &CAT_SPECIES,      &DRAGON_SPECIES,  &OCTOPUS_SPECIES, &OWL_SPECIES,
  &PENGUIN_SPECIES,  &TURTLE_SPECIES,  &SNAIL_SPECIES,   &GHOST_SPECIES,
  &AXOLOTL_SPECIES,  &CACTUS_SPECIES,  &ROBOT_SPECIES,   &RABBIT_SPECIES,
  &MUSHROOM_SPECIES, &CHONK_SPECIES,
};
static const uint8_t N_SPECIES = sizeof(SPECIES_TABLE) / sizeof(SPECIES_TABLE[0]);
static uint8_t  currentIdx  = 0;
static uint32_t tickCount   = 0;
static uint32_t nextTickAt  = 0;
static const uint32_t TICK_MS = 200;
static const uint8_t SPECIES_NVS_SENTINEL = 0xFF;   // matches M5 main.cpp

void buddyInit() {
  uint8_t saved = speciesIdxLoad();
  // 0xFF = "use GIF" on the M5 build; here it just means "no saved choice
  // yet" (no GIF mode in this stage), so fall through to default 0.
  if (saved < N_SPECIES) currentIdx = saved;
  tickCount = 0;
  nextTickAt = 0;
}

void buddyInvalidate() {
  // No draw cache on the AMOLED side — main.cpp clears the canvas
  // on every frame via fillSprite(). Kept as a no-op so the buddy.h API
  // stays consistent with the M5 build.
}

void buddySetSpeciesIdx(uint8_t idx) {
  if (idx < N_SPECIES) currentIdx = idx;
}

void buddySetSpecies(const char* name) {
  for (uint8_t i = 0; i < N_SPECIES; i++) {
    if (strcmp(SPECIES_TABLE[i]->name, name) == 0) {
      currentIdx = i;
      return;
    }
  }
}

void buddyNextSpecies() {
  currentIdx = (currentIdx + 1) % N_SPECIES;
  speciesIdxSave(currentIdx);
}

void buddySetPeek(bool) {
  // M5 used this to shrink the buddy when PET/INFO needed screen space;
  // not needed on the 368×448 panel (PET/INFO replace the buddy view
  // entirely).
}

const char* buddySpeciesName() { return SPECIES_TABLE[currentIdx]->name; }
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

// buddy.h declares this; M5 used it for the landscape clock face. On
// AMOLED the clock face is portrait-only and doesn't use the buddy, so
// this is a no-op. The forward declaration of TFT_eSPI keeps buddy.h
// includable without dragging in the M5 graphics library.
class TFT_eSPI;
void buddyRenderTo(TFT_eSPI*, uint8_t) { /* no-op on AMOLED */ }
