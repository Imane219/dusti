#include <ctime>
#include "Mutation.h"
#include "Dictionary.h"
#include "Util.h"
#include "FuzzItem.h"
#include "Logger.h"

using namespace std;
using namespace fuzzer;

uint64_t Mutation::stageCycles[32] = {};

Mutation::Mutation(FuzzItem item, Dicts dicts): curFuzzItem(item), dicts(dicts), dataSize(item.data.size()) {
  effCount = 0;
  eff = bytes(effALen(dataSize), 0);
  eff[0] = 1;
  if (effAPos(dataSize - 1) != 0) {
    eff[effAPos(dataSize - 1)] = 1;
    effCount ++;
  }
  stageName = "init";
}

void Mutation::flipbit(int pos) {
  curFuzzItem.data[pos >> 3] ^= (128 >> (pos & 7));
}

//bitflip 1/1:每次翻转1个bit，按照每1个bit的步长从头开始
void Mutation::singleWalkingBit(OnMutateFunc cb) {
  stageName = "bitflip 1/1";
  stageMax = dataSize << 3;
  /* Start fuzzing */
  for (stageCur = 0; stageCur < stageMax ; stageCur += 1) {
    flipbit(stageCur);
    cb(curFuzzItem.data);
    flipbit(stageCur);
  }
  stageCycles[STAGE_FLIP1] += stageMax;
}

//bitflip 2/1:每次翻转相邻的2个bit，按照每1个bit的步长从头开始
void Mutation::twoWalkingBit(OnMutateFunc cb) {
  stageName = "bitflip 2/1";
  stageMax = (dataSize << 3) - 1;
  /* Start fuzzing */
  for (stageCur = 0; stageCur < stageMax; stageCur += 1) {
    flipbit(stageCur);
    flipbit(stageCur + 1);
    cb(curFuzzItem.data);
    flipbit(stageCur);
    flipbit(stageCur + 1);
  }
  stageCycles[STAGE_FLIP2] += stageMax;
}

//bitflip 4/1:每次翻转相邻的4个bit，按照每1个bit的步长从头开始
void Mutation::fourWalkingBit(OnMutateFunc cb) {
  stageName = "bitflip 4/1";
  stageMax = (dataSize << 3) - 3;
  /* Start fuzzing */
  for (stageCur = 0; stageCur < stageMax; stageCur += 1) {
    flipbit(stageCur);
    flipbit(stageCur + 1);
    flipbit(stageCur + 2);
    flipbit(stageCur + 3);
    cb(curFuzzItem.data);
    flipbit(stageCur);
    flipbit(stageCur + 1);
    flipbit(stageCur + 2);
    flipbit(stageCur + 3);
  }
  stageCycles[STAGE_FLIP4] += stageMax;
}

//bitflip 8/8:每次翻转相邻的8个bit，按照每8个bit的步长从头开始，即依次对每个byte做翻转
void Mutation::singleWalkingByte(OnMutateFunc cb) {
  stageName = "bitflip 8/8";
  stageMax = dataSize;
  /* Start fuzzing */
  for (stageCur = 0; stageCur < stageMax; stageCur += 1) {
    curFuzzItem.data[stageCur] ^= 0xFF;
    FuzzItem item = cb(curFuzzItem.data);
    /* We also use this stage to pull off a simple trick: we identify
     bytes that seem to have no effect on the current execution path
     even when fully flipped - and we skip them during more expensive
     deterministic stages, such as arithmetics or known ints. */
    if (!eff[effAPos(stageCur)]) {
      if (item.res.cksum != curFuzzItem.res.cksum) {
        eff[effAPos(stageCur)] = 1;
        effCount += 1;
      }
    }
    curFuzzItem.data[stageCur] ^= 0xFF;
  }
  /* If the effector map is more than EFF_MAX_PERC dense, just flag the
   whole thing as worth fuzzing, since we wouldn't be saving much time
   anyway. */
  if (effCount != effALen(dataSize) && effCount * 100 / effALen(dataSize) > EFF_MAX_PERC) {
    eff = bytes(effALen(dataSize), 1);
  }
  stageCycles[STAGE_FLIP8] += stageMax;
}

//bitflip 16/8:每次翻转相邻的16个bit，按照每8个bit的步长从头开始，即依次对每个word做翻转
void Mutation::twoWalkingByte(OnMutateFunc cb) {
  stageName = "bitflip 16/8";
  stageMax = dataSize - 1;
  stageCur = 0;
  /* Start fuzzing */
  u8 *buf = curFuzzItem.data.data();
  for (int i = 0; i < dataSize - 1; i += 1) {
    /* Let's consult the effector map... */
    if (!eff[effAPos(i)] && !eff[effAPos(i + 1)]) {
      stageMax--;
      continue;
    }
    *(u16*)(buf + i) ^= 0xFFFF;
    cb(curFuzzItem.data);
    stageCur ++;
    *(u16*)(buf + i) ^= 0xFFFF;
  }
  stageCycles[STAGE_FLIP16] += stageMax;
}

//bitflip 32/8:每次翻转相邻的32个bit，按照每8个bit的步长从头开始，即依次对每个dword做翻转
void Mutation::fourWalkingByte(OnMutateFunc cb) {
  stageName = "bitflip 32/8";
  stageMax = dataSize - 3;
  stageCur = 0;
  /* Start fuzzing */
  u8 *buf = curFuzzItem.data.data();
  for (int i = 0; i < dataSize - 3; i += 1) {
    /* Let's consult the effector map... */
    if (!eff[effAPos(i)] && !eff[effAPos(i + 1)] &&
        !eff[effAPos(i + 2)] && !eff[effAPos(i + 3)]) {
      stageMax --;
      continue;
    }
    *(u32*)(buf + i) ^= 0xFFFFFFFF;
    cb(curFuzzItem.data);
    stageCur ++;
    *(u32*)(buf + i) ^= 0xFFFFFFFF;
  }
  stageCycles[STAGE_FLIP32] += stageMax;
}

//arith 8/8:每次对8个bit进行加减运算，按照每8个bit的步长从头开始，
//即对文件的每个byte进行整数加减变异
void Mutation::singleArith(OnMutateFunc cb) {
  stageName = "arith 8/8";
  stageMax = 2 * dataSize * ARITH_MAX;
  stageCur = 0;
  /* Start fuzzing */
  for (int i = 0; i < dataSize; i += 1) {
    /* Let's consult the effector map... */
    if (!eff[effAPos(i)]) {
      stageMax -= (2 * ARITH_MAX);
      continue;
    }
    byte orig = curFuzzItem.data[i];
    for (int j = 1; j <= ARITH_MAX; j += 1) {
      byte r = orig ^ (orig + j);
      if (!couldBeBitflip(r)) {
        curFuzzItem.data[i] = orig + j;
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      r = orig ^ (orig - j);
      if (!couldBeBitflip(r)) {
        curFuzzItem.data[i] = orig - j;
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      curFuzzItem.data[i] = orig;
    }
  }
  stageCycles[STAGE_ARITH8] += stageMax;
}

//arith 16/8，每次对16个bit进行加减运算，按照每8个bit的步长从头开始，
//即对文件的每个word进行整数加减变异
void Mutation::twoArith(OnMutateFunc cb) {
  stageName = "arith 16/8";
  stageMax = 4 * (dataSize - 1) * ARITH_MAX;
  stageCur = 0;
  /* Start fuzzing */
  byte *buf = curFuzzItem.data.data();
  for (int i = 0; i < dataSize - 1; i += 1) {
    u16 orig = *(u16*)(buf + i);
    if (!eff[effAPos(i)] && !eff[effAPos(i + 1)]) {
      stageMax -= 4 * ARITH_MAX;
      continue;
    }
    for (int j = 1; j <= ARITH_MAX; j += 1) {
      u16 r1 = orig ^ (orig + j);
      u16 r2 = orig ^ (orig - j);
      u16 r3 = orig ^ swap16(swap16(orig) + j);
      u16 r4 = orig ^ swap16(swap16(orig) - j);
      if ((orig & 0xFF) + j > 0xFF && !couldBeBitflip(r1)) {
        *(u16*)(buf + i) = orig + j;
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      if ((orig & 0xFF) < j && !couldBeBitflip(r2)) {
        *(u16*)(buf + i) = orig - j;
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      if ((orig >> 8) + j > 0xFF && !couldBeBitflip(r3)) {
        *(u16*)(buf + i) = swap16(swap16(orig) + j);
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      if ((orig >> 8) < j && !couldBeBitflip(r4)) {
        *(u16*)(buf + i) = swap16(swap16(orig) - j);
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      *(u16*)(buf + i) = orig;
    }
  }
  stageCycles[STAGE_ARITH16] += stageMax;
}

//arith 32/8，每次对32个bit进行加减运算，按照每8个bit的步长从头开始，
//即对文件的每个dword进行整数加减变异
void Mutation::fourArith(OnMutateFunc cb) {
  stageName = "arith 32/8";
  stageMax = 4 * (dataSize - 3) * ARITH_MAX;
  stageCur = 0;
  /* Start fuzzing */
  byte *buf = curFuzzItem.data.data();
  for (int i = 0; i < dataSize - 3; i += 1) {
    u32 orig = *(u32*)(buf + i);
    /* Let's consult the effector map... */
    if (!eff[effAPos(i)] && !eff[effAPos(i + 1)] && !eff[effAPos(i + 2)] && !eff[effAPos(i + 3)]) {
      stageMax -= 4 * ARITH_MAX;
      continue;
    }
    for (int j = 1; j <= ARITH_MAX; j += 1) {
      u32 r1 = orig ^ (orig + j);
      u32 r2 = orig ^ (orig - j);
      u32 r3 = orig ^ swap32(swap32(orig) + j);
      u32 r4 = orig ^ swap32(swap32(orig) - j);
      if ((orig & 0xFFFF) + j > 0xFFFF && !couldBeBitflip(r1)) {
        *(u32*)(buf + i) = orig + j;
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      if ((orig & 0xFFFF) < (u32)j && !couldBeBitflip(r2)) {
        *(u32*)(buf + i) = orig - j;
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      if ((swap32(orig) & 0xFFFF) + j > 0xFFFF && !couldBeBitflip(r3)) {
        *(u32*)(buf + i) = swap32(swap32(orig) + j);
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      if ((swap32(orig) & 0xFFFF) < (u32) j && !couldBeBitflip(r4)) {
        *(u32*)(buf + i) = swap32(swap32(orig) - j);
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      *(u32*)(buf + i) = orig;
    }
  }
  stageCycles[STAGE_ARITH32] += stageMax;
}

//interest 8/8:每次对8个bit进替换，按照每8个bit的步长从头开始，即对文件的每个byte进行替换
void Mutation::singleInterest(OnMutateFunc cb) {
  stageName = "interest 8/8";
  stageMax = dataSize * sizeof(INTERESTING_8);
  stageCur = 0;
  /* Start fuzzing */
  for (int i = 0; i < dataSize; i += 1) {
    u8 orig = curFuzzItem.data[i];
    /* Let's consult the effector map... */
    if (!eff[effAPos(i)]) {
      stageMax -= sizeof(INTERESTING_8);
      continue;
    }
    for (int j = 0; j < (int) sizeof(INTERESTING_8); j += 1) {
      if (couldBeBitflip(orig ^ (u8)INTERESTING_8[j]) || couldBeArith(orig, (u8)INTERESTING_8[j], 1)) {
        stageMax --;
        continue;
      }
      curFuzzItem.data[i] = INTERESTING_8[j];
      cb(curFuzzItem.data);
      stageCur ++;
      curFuzzItem.data[i] = orig;
    }
  }
  stageCycles[STAGE_INTEREST8] += stageMax;
}

//interest 16/8:每次对16个bit进替换，按照每8个bit的步长从头开始，即对文件的每个word进行替换
void Mutation::twoInterest(OnMutateFunc cb) {
  stageName = "interest 16/8";
  stageMax = 2 * (dataSize - 1) * (sizeof(INTERESTING_16) >> 1);
  stageCur = 0;
  /* Start fuzzing */
  byte *out_buf = curFuzzItem.data.data();
  for (int i = 0; i < dataSize - 1; i += 1) {
    u16 orig = *(u16*)(out_buf + i);
    if (!eff[effAPos(i)] && !eff[effAPos(i + 1)]) {
      stageMax -= sizeof(INTERESTING_16);
      continue;
    }
    for (int j = 0; j < (int) sizeof(INTERESTING_16) / 2; j += 1) {
      if (!couldBeBitflip(orig ^ (u16)INTERESTING_16[j]) &&
          !couldBeArith(orig, (u16)INTERESTING_16[j], 2) &&
          !couldBeInterest(orig, (u16)INTERESTING_16[j], 2, 0)) {
        *(u16*)(out_buf + i) = INTERESTING_16[j];
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;

      if ((u16)INTERESTING_16[j] != swap16(INTERESTING_16[j]) &&
          !couldBeBitflip(orig ^ swap16(INTERESTING_16[j])) &&
          !couldBeArith(orig, swap16(INTERESTING_16[j]), 2) &&
          !couldBeInterest(orig, swap16(INTERESTING_16[j]), 2, 1)) {
        *(u16*)(out_buf + i) = swap16(INTERESTING_16[j]);
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
    }
    *(u16*)(out_buf + i) = orig;
  }
  stageCycles[STAGE_INTEREST16] += stageMax;
}

//interest 32/8:每次对32个bit进替换，按照每8个bit的步长从头开始，即对文件的每个dword进行替换
void Mutation::fourInterest(OnMutateFunc cb) {
  stageName = "interest 32/8";
  stageMax = 2 * (dataSize - 3) * (sizeof(INTERESTING_32) >> 2);
  stageCur = 0;
  /* Start fuzzing */
  byte *out_buf = curFuzzItem.data.data();
  for (int i = 0; i < dataSize - 3; i++) {
    u32 orig = *(u32*)(out_buf + i);
    /* Let's consult the effector map... */
    if (!eff[effAPos(i)] && !eff[effAPos(i + 1)] &&
        !eff[effAPos(i + 2)] && !eff[effAPos(i + 3)]) {
      stageMax -= sizeof(INTERESTING_32) >> 1;
      continue;
    }
    for (int j = 0; j < (int) sizeof(INTERESTING_32) / 4; j++) {
      /* Skip if this could be a product of a bitflip, arithmetics,
       or word interesting value insertion. */
      if (!couldBeBitflip(orig ^ (u32)INTERESTING_32[j]) &&
          !couldBeArith(orig, INTERESTING_32[j], 4) &&
          !couldBeInterest(orig, INTERESTING_32[j], 4, 0)) {
        *(u32*)(out_buf + i) = INTERESTING_32[j];
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
      if ((u32)INTERESTING_32[j] != swap32(INTERESTING_32[j]) &&
          !couldBeBitflip(orig ^ swap32(INTERESTING_32[j])) &&
          !couldBeArith(orig, swap32(INTERESTING_32[j]), 4) &&
          !couldBeInterest(orig, swap32(INTERESTING_32[j]), 4, 1)) {
        *(u32*)(out_buf + i) = swap32(INTERESTING_32[j]);
        cb(curFuzzItem.data);
        stageCur ++;
      } else stageMax --;
    }
    *(u32*)(out_buf + i) = orig;
  }
  stageCycles[STAGE_INTEREST32] += stageMax;
}

//值字典变异
void Mutation::overwriteWithDictionary(OnMutateFunc cb) {
  stageName = "dict (over)";
  auto dict = get<0>(dicts);
  stageMax = dataSize * dict.extras.size();
  stageCur = 0;
  /* Start fuzzing */
  byte *outBuf = curFuzzItem.data.data();
  byte inBuf[curFuzzItem.data.size()];
  memcpy(inBuf, outBuf, curFuzzItem.data.size());
  u32 extrasCount = dict.extras.size();
  /*
   * In solidity - data block is 32 bytes then change to step = 32, not 1
   * Size of extras is alway 32
   */
  for (u32 i = 0; i < (u32)dataSize; i += 1) {
    u32 lastLen = 0;
    for (u32 j = 0; j < extrasCount; j += 1) {
      byte *extrasBuf = dict.extras[j].data.data();
      byte *effBuf = eff.data();
      u32 extrasLen = dict.extras[j].data.size();
      /* Skip extras probabilistically if extras_cnt > MAX_DET_EXTRAS. Also
       skip them if there's no room to insert the payload, if the token
       is redundant, or if its entire span has no bytes set in the effector
       map. */
      if ((extrasCount > MAX_DET_EXTRAS
          && UR(extrasCount) > MAX_DET_EXTRAS)
          || extrasLen > (dataSize - i)
          || !memcmp(extrasBuf, outBuf + i, extrasLen)
          || !memchr(effBuf + effAPos(i), 1, effSpanALen(i, extrasLen))
          ) {
        stageMax --;
        continue;
      }
      lastLen = extrasLen;
      memcpy(outBuf + i, extrasBuf, lastLen);
      cb(curFuzzItem.data);
      stageCur ++;
    }
    /* Restore all the clobbered memory. */
    memcpy(outBuf + i, inBuf + i, lastLen);
  }
  stageCycles[STAGE_EXTRAS_UO] += stageMax;
}

//地址变异
void Mutation::overwriteWithAddressDictionary(OnMutateFunc cb) {
  stageName = "address (over)";
  auto dict = get<1>(dicts);

  stageMax = (dataSize / 32) * dict.extras.size();
  stageCur = 0;
  /* Start fuzzing */
  byte *outBuf = curFuzzItem.data.data();
  byte inBuf[curFuzzItem.data.size()];
  memcpy(inBuf, outBuf, curFuzzItem.data.size());
  u32 extrasCount = dict.extras.size();
  u32 extrasLen = 20;
  for (u32 i = 0; i < (u32)dataSize; i += 32) {
    for (u32 j = 0; j < extrasCount; j += 1) {
      byte *extrasBuf = dict.extras[j].data.data();
      if (!memcmp(extrasBuf, outBuf + i + 12, extrasLen)) {
        stageMax --;
        continue;
      }
      memcpy(outBuf + i + 12, extrasBuf, extrasLen);
      cb(curFuzzItem.data);
      stageCur ++;
    }
    /* Restore all the clobbered memory. */
    memcpy(outBuf + i, inBuf + i, 32);
  }
  stageCycles[STAGE_EXTRAS_AO] += stageMax;
}

/*
 * TODO: If found more, do more havoc
 */
//大破坏: 大量变异
void Mutation::havoc(OnMutateFunc cb) {
  stageName = "havoc";
  stageMax = HAVOC_MIN;
  stageCur = 0;

  auto dict = get<0>(dicts);
  auto origin = curFuzzItem.data;
  bytes data = origin;
  for (int i = 0; i < HAVOC_MIN; i += 1) {
    u32 useStacking = 1 << (1 + UR(HAVOC_STACK_POW2));
    for (u32 j = 0; j < useStacking; j += 1) {
      u32 val = UR(11 + ((dict.extras.size() + 0) ? 2 : 0));
      dataSize = data.size();
      byte *out_buf = data.data();
      switch (val) {
        case 0: {
          /* Flip a single bit somewhere. Spooky! */
          u32 pos = UR(dataSize << 3);
          data[pos >> 3] ^= (128 >> (pos & 7));
          break;
        }
        case 1: {
          /* Set byte to interesting value. */
          data[UR(dataSize)] = INTERESTING_8[UR(sizeof(INTERESTING_8))];
          break;
        }
        case 2: {
          /* Set word to interesting value, randomly choosing endian. */
          if (dataSize < 2) break;
          if (UR(2)) {
            *(u16*)(out_buf + UR(dataSize - 1)) = INTERESTING_16[UR(sizeof(INTERESTING_16) >> 1)];
          } else {
            *(u16*)(out_buf + UR(dataSize - 1)) = swap16(INTERESTING_16[UR(sizeof(INTERESTING_16) >> 1)]);
          }
          break;
        }
        case 3: {
          /* Set dword to interesting value, randomly choosing endian. */
          if (dataSize < 4) break;
          if (UR(2)) {
            *(u32*)(out_buf + UR(dataSize - 3)) = INTERESTING_32[UR(sizeof(INTERESTING_32) >> 2)];
          } else {
            *(u32*)(out_buf + UR(dataSize - 3)) = swap32(INTERESTING_32[UR(sizeof(INTERESTING_32) >> 2)]);
          }
          break;
        }
        case 4: {
          /* Randomly subtract from byte. */
          out_buf[UR(dataSize)] -= 1 + UR(ARITH_MAX);
          break;
        }
        case 5: {
          /* Randomly add to byte. */
          out_buf[UR(dataSize)] += 1 + UR(ARITH_MAX);
          break;
        }
        case 6: {
          /* Randomly subtract from word, random endian. */
          if (dataSize < 2) break;
          if (UR(2)) {
            u32 pos = UR(dataSize - 1);
            *(u16*)(out_buf + pos) -= 1 + UR(ARITH_MAX);
          } else {
            u32 pos = UR(dataSize - 1);
            u16 num = 1 + UR(ARITH_MAX);
            *(u16*)(out_buf + pos) = swap16(swap16(*(u16*)(out_buf + pos)) - num);
          }
          break;
        }
        case 7: {
          /* Randomly add to word, random endian. */
          if (dataSize < 2) break;
          if (UR(2)) {
            u32 pos = UR(dataSize - 1);
            *(u16*)(out_buf + pos) += 1 + UR(ARITH_MAX);
          } else {
            u32 pos = UR(dataSize - 1);
            u16 num = 1 + UR(ARITH_MAX);
            *(u16*)(out_buf + pos) = swap16(swap16(*(u16*)(out_buf + pos)) + num);
          }
          break;
        }
        case 8: {
          /* Randomly subtract from dword, random endian. */
          if (dataSize < 4) break;
          if (UR(2)) {
            u32 pos = UR(dataSize - 3);
            *(u32*)(out_buf + pos) -= 1 + UR(ARITH_MAX);
          } else {
            u32 pos = UR(dataSize - 3);
            u32 num = 1 + UR(ARITH_MAX);
            *(u32*)(out_buf + pos) = swap32(swap32(*(u32*)(out_buf + pos)) - num);
          }
          break;
        }
        case 9: {
          /* Randomly add to dword, random endian. */
          if (dataSize < 4) break;
          if (UR(2)) {
            u32 pos = UR(dataSize - 3);
            *(u32*)(out_buf + pos) += 1 + UR(ARITH_MAX);
          } else {
            u32 pos = UR(dataSize - 3);
            u32 num = 1 + UR(ARITH_MAX);
            *(u32*)(out_buf + pos) = swap32(swap32(*(u32*)(out_buf + pos)) + num);
          }
          break;
        }
        case 10: {
          /* Just set a random byte to a random value. Because,
           why not. We use XOR with 1-255 to eliminate the
           possibility of a no-op. */
          out_buf[UR(dataSize)] ^= 1 + UR(255);
          break;
        }
        case 11: {
          /* Overwrite bytes with a randomly selected chunk (75%) or fixed
           bytes (25%). */
          u32 copyFrom, copyTo, copyLen;
          if (dataSize < 2) break;
          copyLen = chooseBlockLen(dataSize - 1);
          copyFrom = UR(dataSize - copyLen + 1);
          copyTo = UR(dataSize - copyLen + 1);
          if (UR(4)) {
            if (copyFrom != copyTo)
              memmove(out_buf + copyTo, out_buf + copyFrom, copyLen);
          } else {
            memset(out_buf + copyTo, UR(2) ? UR(256) : out_buf[UR(dataSize)], copyLen);
          }
          break;
        }
        case 12: {
          /* No auto extras or odds in our favor. Use the dictionary. */
          u32 useExtra = UR(dict.extras.size());
          u32 extraLen = dict.extras[useExtra].data.size();
          byte *extraBuf = dict.extras[useExtra].data.data();
          u32 insertAt;
          if (extraLen > (u32)dataSize) break;
          insertAt = UR(dataSize - extraLen + 1);
          memcpy(out_buf + insertAt, extraBuf, extraLen);
          break;
        }
      }
    }
    cb(data);
    stageCur ++;
    /* Restore to original state */
    data = origin;
  }
  stageCycles[STAGE_HAVOC] += stageMax;
}

//拼接
bool Mutation::splice(vector<FuzzItem> queues) {
  u32 spliceCycle = 0;
  s32 firstDiff, lastDiff;
  bytes origin = curFuzzItem.data;
  if (queues.size() <= 1) return false;
  auto numDiff = count_if(queues.begin(), queues.end(), [&](const FuzzItem &item) {
    return item.res.cksum != queues[0].res.cksum;
  });
  if (!numDiff) return false;
  while (spliceCycle++ < SPLICE_CYCLES && curFuzzItem.data.size() > 1) {
    u32 tid, splitAt;
    do {
      tid = UR(queues.size());
    } while (queues[tid].res.cksum == curFuzzItem.res.cksum);
    FuzzItem target = queues[tid];
    /* Find a suitable splicing location, somewhere between the first and
     the last differing byte. Bail out if the difference is just a single
     byte or so. */
    byte *outBuf = curFuzzItem.data.data();
    byte *targetBuf = target.data.data();
    u32 minLen = curFuzzItem.data.size() > target.data.size()
    ? target.data.size() : curFuzzItem.data.size();
    locateDiffs(outBuf, targetBuf, minLen, &firstDiff, &lastDiff);
    if (firstDiff < 0 || lastDiff < 2 || firstDiff == lastDiff) {
      continue;
    }
    splitAt = firstDiff + UR(lastDiff - firstDiff);
    /* Do the thing. */
    memcpy(outBuf, targetBuf, splitAt);
    return true;
  }
  return false;
}

//随机生成
void Mutation::random(OnMutateFunc cb) {
  stageName = "random 8/8";
  stageMax = 1;
  for (int i = 0; i < dataSize; i ++) {
    curFuzzItem.data[stageCur] = UR(256);
  }
  cb(curFuzzItem.data);
  stageCycles[STAGE_RANDOM] += stageMax;
}
