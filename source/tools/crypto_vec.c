/* prints test vectors for tools/test_crypto.py */
#include "../src/crypto.h"
#include <stdio.h>
#include <stdlib.h>
static void hex(const uint8_t *p, size_t n){for(size_t i=0;i<n;i++)printf("%02x",p[i]);printf("\n");}
int main(int argc,char**argv){
  /* input: deterministic bytes */
  static uint8_t d[70000]; for(int i=0;i<70000;i++) d[i]=(uint8_t)(i*31+7);
  size_t lens[]={0,1,55,56,63,64,135,136,137,1000,70000};
  uint8_t o[32];
  for(int i=0;i<11;i++){sha256(d,lens[i],o);printf("sha256 %zu ",lens[i]);hex(o,32);}
  for(int i=0;i<11;i++){sha3_256(d,lens[i],o);printf("sha3 %zu ",lens[i]);hex(o,32);}
  hmac_sha256(d,32,d+100,20,o);printf("hmac32 ");hex(o,32);
  hmac_sha256(d,100,d+100,500,o);printf("hmac100 ");hex(o,32);
  aes128_ctx a; aes128_init(&a,d+5); uint8_t b[16]; aes128_encrypt(&a,d+40,b);printf("aesenc ");hex(b,16);
  aes128_decrypt(&a,d+40,b);printf("aesdec ");hex(b,16);
  xts_ctx x; xts_init(&x,d+7,d+23);
  static uint8_t s[0x1000]; for(int i=0;i<0x1000;i++) s[i]=d[i+300];
  xts_decrypt(&x,s,0x1000,0x800000000069ULL); printf("xtsdec ");sha256(s,0x1000,o);hex(o,32);
  for(int i=0;i<0x1000;i++) s[i]=d[i+300];
  xts_encrypt(&x,s,0x1000,5); printf("xtsenc ");sha256(s,0x1000,o);hex(o,32);
  return 0;}
