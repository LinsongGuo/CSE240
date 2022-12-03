//========================================================//
//  predictor.c                                           //
//  Source file for the Branch Predictor                  //
//                                                        //
//  Implement the various branch predictors below as      //
//  described in the README                               //
//========================================================//
#include <stdio.h>
#include "predictor.h"

#include <stdlib.h>
#include <stdbool.h>

//
// TODO:Student Information
//
const char *studentName = "NAME";
const char *studentID   = "PID";
const char *email       = "EMAIL";

//------------------------------------//
//      Predictor Configuration       //
//------------------------------------//

// Handy Global for use in output routines
const char *bpName[4] = { "Static", "Gshare",
                          "Tournament", "Custom" };

int ghistoryBits; // Number of bits used for Global History
int lhistoryBits; // Number of bits used for Local History
int pcIndexBits;  // Number of bits used for PC index
int bpType;       // Branch Prediction Type
int verbose;

//------------------------------------//
//      Predictor Data Structures     //
//------------------------------------//

//
//TODO: Add your own Branch Predictor data structures here
//

uint32_t ghistoryMask, lhistoryMask, pcIndexMask; 
// Bit masks for ghistoryBits, lhistoryBits and pcIndexBits

// history register type
typedef struct hreg_t_ {
  // unsigned int value : 10;
  unsigned int value;
} hreg_t;

hreg_t GHR; // global history register
hreg_t* LHT; // local history table 

// 2-bits saturating counter
typedef struct _counter_t {
  bool bits[2];
} counter_t;

counter_t* GPT; // gloabl prediction table
counter_t* LPT; // local prediction table
counter_t* chooser; // choose prediction table

uint8_t local_predict, global_predict;

// For custom:
#define bimodalBits 12
#define bankBits 10
uint32_t bimodalMask;
uint64_t bankMask, bankMask2, bankMask4;

const uint32_t csr1 = 6, csr2 = 5;

typedef struct _counter3_t {
  unsigned int value : 3; 
} counter3_t;

struct bimodal_t {
  counter3_t ctrs[1 << bimodalBits];
  bool m[1 << bimodalBits];
} bank0;

struct bank_t {
  counter3_t ctrs[1 << bankBits];
  bool u[1 << bankBits];
  uint8_t tags[1 << bankBits]; 
} bank[4];

uint64_t ghr0, ghr1;
bool flags[5];
uint8_t predict[5], X;
uint32_t tags[5], idx[5];
//------------------------------------//
//        Predictor Functions         //
//------------------------------------//

// update global history register
void update_GHR(uint8_t outcome) {
  if (outcome) {
    GHR.value = GHR.value << 1 | 1;
  }
  else {
    GHR.value = GHR.value << 1;
  }
  GHR.value &= ghistoryMask;
}

// update local history register for a pc
void update_LTH(uint32_t pc, uint8_t outcome) {
  uint32_t index = pc & pcIndexMask;
  if (outcome) {
    LHT[index].value = LHT[index].value << 1 | 1;
  }
  else {
    LHT[index].value = LHT[index].value << 1;
  }
  LHT[index].value &= lhistoryMask;
}

counter_t counter_update(counter_t counter, uint8_t outcome) {
  if (outcome == TAKEN) {
    if (counter.bits[1]) {
      if (!counter.bits[0]) { // 10 Weakly Taken
        counter.bits[0] = 1; // to 11 Stronly Taken
      }
    }
    else {
      if (counter.bits[0]) { // 01 Weakly Not-Taken
        counter.bits[0] = 0; 
        counter.bits[1] = 1; // to 10 Weakly Taken
      }
      else {  // 00 Strongly Not-Taken
        counter.bits[0] = 1; // to 01 Weakly Not-Taken
      }
    }
  }
  else {
    if (counter.bits[1]) {
      if (counter.bits[0]) { // 11 Strongly Taken 
        counter.bits[0] = 0; // to 10 Weakly Taken
      }
      else { // 10 Weakly Taken
        counter.bits[0] = 1;
        counter.bits[1] = 0; // to 01 Weakly Not-Taken
      }
    }
    else {
      if (counter.bits[0]) { // 01 Weakly Not-Taken
        counter.bits[0] = 0; // to 00 Strongly Not-Taken
      }
    }
  }
  return counter;
}

uint8_t counter_predict(counter_t counter) {
  return counter.bits[1];
}

counter3_t counter3_update(counter3_t counter, uint8_t outcome) {
  if (outcome == TAKEN) {
    if (counter.value < 7) {
      counter.value += 1;
    }
  }
  else {
    if (counter.value > 0) {
      counter.value -= 1;
    }
  }
  return counter;
}

uint8_t counter3_predict(counter3_t counter) {
  return (counter.value >> 2) & 1;
}

// Functions For Gshare
void gshare_init() {
  ghistoryMask = (1 << ghistoryBits) - 1;
  GHR.value = 0;
  GPT = (counter_t*) malloc((1 << ghistoryBits) * sizeof(counter_t));
  int i;
  for (i = 0; i <= ghistoryMask; ++i) {
    GPT[i].bits[0] = GPT[i].bits[1] = 0;
  }
}

uint8_t gshare_predict(uint32_t pc) {
  uint32_t index = GHR.value ^ (pc & ghistoryMask);
  return counter_predict(GPT[index]);
}

void gshare_train(uint32_t pc, uint8_t outcome) {
  uint32_t index = GHR.value ^ (pc & ghistoryMask);
  GPT[index] = counter_update(GPT[index], outcome);
  update_GHR(outcome);
}

// Functions For Tournament.
void tournament_init() {
  ghistoryMask = (1 << ghistoryBits) - 1;
  lhistoryMask = (1 << lhistoryBits) - 1;
  pcIndexMask = (1 << pcIndexBits) - 1; 

  GHR.value = 0;
  GPT = (counter_t*) malloc((1 << ghistoryBits) * sizeof(counter_t));
  chooser = (counter_t*) malloc((1 << ghistoryBits) * sizeof(counter_t));
  LHT = (hreg_t*) malloc((1 << pcIndexBits) * sizeof(hreg_t));
  LPT = (counter_t*) malloc((1 << lhistoryBits) * sizeof(counter_t));
  
  int i;
  for (i = 0; i <= ghistoryMask; ++i) {
    GPT[i].bits[0] = GPT[i].bits[1] = 0;
    chooser[i].bits[0] = chooser[i].bits[1] = 0;
  }
  for (i = 0; i <= pcIndexMask; ++i) {
    LHT[i].value = 0;
  }
  for (i = 0; i <= lhistoryMask; ++i) {
    LPT[i].bits[0] = LPT[i].bits[1] = 0;
  }
}

uint8_t tournament_predict(uint32_t pc) {
  uint32_t local_index = LHT[pc & pcIndexMask].value;
  local_predict = counter_predict(LPT[local_index]);

  uint32_t global_index = GHR.value;
  global_predict = counter_predict(GPT[global_index]);

  uint8_t choose = counter_predict(chooser[global_index]);

  // default : local
  // return choose == TAKEN ? local_predict : global_predict;
  // default : global
  return choose == TAKEN ? global_predict : local_predict;
}

void tournament_train(uint32_t pc, uint8_t outcome) {
  uint32_t local_index = LHT[pc & pcIndexMask].value;
  LPT[local_index] = counter_update(LPT[local_index], outcome);
  update_LTH(pc, outcome);

  uint32_t global_index =  GHR.value;
  GPT[global_index] = counter_update(GPT[global_index], outcome);
  update_GHR(outcome);

  if (local_predict != global_predict) {
    // chooser[global_index] = counter_update(chooser[global_index], local_predict == outcome); // default : local
    chooser[global_index] = counter_update(chooser[global_index], global_predict == outcome); // default : global
  }
}

// Functions for custom predictors
void custom_init() {
  ghr0 = ghr1 = 0;
  
  bimodalMask = (1 << bimodalBits) - 1;
  bankMask = (1LL << bankBits) - 1;
  bankMask2 = (1LL << (bankBits << 1)) - 1;
  bankMask4 = (1LL << (bankBits << 2)) - 1;
  
  int i, j;
  for (i = 0; i < (1 << bimodalBits); ++i) {
    bank0.m[i] = 0;
    bank0.ctrs[i].value = 0;
  }
  for (i = 0; i < 4; ++i) {
    for (j = 0; j < (1 << bankBits); ++j) {
      bank[i].ctrs[j].value = 0;
      bank[i].tags[j] = 0;
      bank[i].u[j] = 0;
    }
  }
}

uint32_t computed_tag(uint32_t pc) {
  return (pc & 255) ^ (ghr0 & 255); 
}

uint8_t custom_predict(uint32_t pc) {
  uint32_t idx0 = pc & bimodalMask;
  predict[0] = counter3_predict(bank0.ctrs[idx0]);
  flags[0] = bank0.m[idx0];
  idx[0] = idx0;

  uint32_t index = (pc & bankMask) ^ ((pc >> bankBits) & bankMask);
  uint32_t ctag = computed_tag(pc);

  index ^= (ghr0 & bankMask);
  predict[1] = counter3_predict(bank[0].ctrs[index]);
  flags[1] = bank[0].u[index];
  tags[1] = bank[0].tags[index];
  idx[1] = index;

  index ^= ((ghr0 >> bankBits) & bankMask);
  predict[2] = counter3_predict(bank[1].ctrs[index]);
  flags[2] = bank[1].u[index];
  tags[2] = bank[1].tags[index];
  idx[2] = index;

  index ^= ((ghr0 >> (bankBits * 2)) & bankMask);
  index ^= ((ghr0 >> (bankBits * 3)) & bankMask);
  predict[3] = counter3_predict(bank[2].ctrs[index]);
  flags[3] = bank[2].u[index];
  tags[3] = bank[2].tags[index];
  idx[3] = index;

  index ^= (ghr1 & bankMask);
  index ^= ((ghr1 >> bankBits) & bankMask);
  index ^= ((ghr1 >> (bankBits * 2)) & bankMask);
  index ^= ((ghr1 >> (bankBits * 3)) & bankMask);
  predict[4] = counter3_predict(bank[3].ctrs[index]);
  flags[4] = bank[3].u[index];
  tags[4] = bank[3].tags[index];
  idx[4] = index;

  X = 5; 
  int i;
  for (i = 4; i >= 1; --i) {
    if (tags[i] == ctag) {
      X = i;
      break; 
    }
  }
  if (X == 5) {
    X = 0;
  }
  return predict[X];
}

void custom_train(uint32_t pc, uint8_t outcome) {
  uint32_t ctag = computed_tag(pc);

  ghr1 = (ghr1 << 1 | (ghr0 >> (bankBits * 4 - 1))) & bankMask4;
  ghr0 = (ghr0 << 1 | outcome) & bankMask4;
  if (X == 0) {
    bank0.ctrs[idx[0]] = counter3_update(bank0.ctrs[idx[0]], outcome);
  }
  else {
    bank[X - 1].ctrs[idx[X]] = counter3_update(bank[X - 1].ctrs[idx[X]], outcome);
  }

  int i;
  if (predict[X] != outcome && X <= 3) {
    bool all_set = true;
    for (i = X + 1; i <= 4; ++i) {
      if (!flags[i]) {
        all_set = false;
      }
    }

    uint8_t Y[5]; 
    Y[0] = 0;
    if (all_set) {
      Y[++Y[0]] = rand() % (4 - X) + X + 1;
    }
    else {
      for (i = X + 1; i <= 4; ++i) {
        if (!flags[i]) {
          Y[++Y[0]] = i;
        }
      }
    }

    counter3_t stolen_ctr = {tags[0] ? (outcome ? 4 : 3) : (predict[0] ? 4 : 3)};
    for (i = 1; i <= Y[0]; ++i) {
      uint8_t y = Y[i];
      bank[y - 1].ctrs[idx[y]] = stolen_ctr;
      bank[y - 1].u[idx[y]] = 0;
      bank[y - 1].tags[idx[y]] = ctag;
    }
  }

  if (X > 0 && predict[X] != predict[0]) {
    if (predict[X] == outcome) {
      bank[X - 1].u[idx[X]] = 1;
      bank0.m[idx[0]] = 1;
    }
    else {
      bank[X - 1].u[idx[X]] = 0;
      bank0.m[idx[0]] = 0;
    }
  }
}

// Initialize the predictor
//
void
init_predictor()
{
  //
  //TODO: Initialize Branch Predictor Data Structures
  //

  switch (bpType) {
    case STATIC:
      break;
    case GSHARE:
      gshare_init();
      break;
    case TOURNAMENT:
      tournament_init();
      break;
    case CUSTOM:
      custom_init();
      break;
    default:
      break;
  }

}

// Make a prediction for conditional branch instruction at PC 'pc'
// Returning TAKEN indicates a prediction of taken; returning NOTTAKEN
// indicates a prediction of not taken
//
uint8_t
make_prediction(uint32_t pc)
{
  //
  //TODO: Implement prediction scheme
  //

  // Make a prediction based on the bpType
  switch (bpType) {
    case STATIC:
      return TAKEN;
    case GSHARE:
      return gshare_predict(pc);
    case TOURNAMENT:
      return tournament_predict(pc);
    case CUSTOM:
      return custom_predict(pc);
    default:
      break;
  }

  // If there is not a compatable bpType then return NOTTAKEN
  return NOTTAKEN;
}

// Train the predictor the last executed branch at PC 'pc' and with
// outcome 'outcome' (true indicates that the branch was taken, false
// indicates that the branch was not taken)
//
void
train_predictor(uint32_t pc, uint8_t outcome)
{
  //
  //TODO: Implement Predictor training
  //

  switch (bpType) {
    case STATIC:
      break;
    case GSHARE:
      gshare_train(pc, outcome);
      break;
    case TOURNAMENT:
      tournament_train(pc, outcome);
      break;
    case CUSTOM:
      custom_train(pc, outcome);
      break;
    default:
      break;
  }

}
