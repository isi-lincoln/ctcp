#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "util.h"
#include "md5.h"

#define UNIT 10

#define MIN(x,y) (y)^(((x) ^ (y)) &  - ((x) < (y))) 

void
write_lines(int lines){
  FILE *shrimp;
  shrimp = fopen("test_file", "wb+");
  int i = 0;

  for(i = 0; i < lines; i++){
     fprintf(shrimp, "%i ", i);
  }

  fprintf(shrimp, "%c\n", '\0');

  fclose(shrimp);
}

void
read_lines(int size, int times){
  FILE *shrimp;
  char *buf = (char*) malloc(sizeof(size));
  size_t bytes_read;
  shrimp = fopen("test_file", "r");

  // Get file length
  fseek(shrimp, 0, SEEK_END);
  long file_size = ftell(shrimp);
  rewind(shrimp);

  bytes_read = fread(buf, sizeof(char), file_size, shrimp);
  fprintf(stdout, "The file size is: %ld\n", file_size);
  fprintf(stdout, "Read %Zd bytes \n", bytes_read);
  fclose(shrimp);
  free(buf);
}

void
copy_file(char* file_name){
  FILE *src;
  FILE *dst;
  char *copy = "2";
  char* new_name = malloc(sizeof(file_name) + sizeof(copy));

  char *buf_1 = malloc(UNIT+1);
  if(!buf_1){ 
    fprintf(stderr, "Memory error. Exiting\n");
    exit(1);
  }

  sprintf(new_name, "%s%s", file_name, copy);
 
  src = fopen(file_name, "rb");
  
  if(!src){
    fprintf(stderr, "Unable to open file %s\n", file_name);
    exit(1);
  }

  dst = fopen(new_name, "wb");

  if(!src){
    fprintf(stderr, "Unable to open file %s\n", new_name);
    fclose(src);
    exit(1);
  }  

  while(!feof(src)){
    size_t bytes_read = fread(buf_1, 1, UNIT, src);
    
    fwrite(buf_1, 1, MIN(UNIT,bytes_read), dst);
  }

  fclose(src);
  fclose(dst);
  free(new_name);
  free(buf_1);
}

void
marshall_test(){
  Data_Pckt* data_packet = dataPacket(1, 2, 3);
  Data_Pckt data_packet_rcv;
  char* buf = malloc(1500);
  
  data_packet->packet_coeff[0] = random()%256;
  data_packet->packet_coeff[1] = random()%256;
  data_packet->packet_coeff[2] = random()%256;

  data_packet->flag = PARTIAL_BLK;
  data_packet->blk_len = 23;
  
  memset(data_packet->payload, 0, PAYLOAD_SIZE);

  data_packet->payload = "This is a test\n";

  marshallData(*data_packet, buf);

  unmarshallData(&data_packet_rcv, buf);
  printf("*** Data Marshalling ***\n");

  printf("tstamp: %f\n", data_packet->tstamp);
  printf("flag: %d\n", data_packet->flag);
  printf("block length: %d\n", data_packet->blk_len);
  printf("seqno: %d\n", data_packet->seqno);
  printf("blockno: %d\n", data_packet->blockno);
  printf("numpackets: %d\n", data_packet->num_packets);
    
  int i;
  for(i = 0; i < data_packet->num_packets; i++){
    printf("coding coeff %d: %d\n", i, data_packet->packet_coeff[i]);
  } 
  
  printf("payload: %s\n", data_packet->payload);

  assert(data_packet->tstamp == data_packet_rcv.tstamp);
  assert(data_packet->flag == data_packet_rcv.flag);
  assert(data_packet->blk_len == data_packet_rcv.blk_len);
  assert(data_packet->seqno == data_packet_rcv.seqno);
  assert(data_packet->blockno == data_packet_rcv.blockno);
  assert(data_packet->num_packets == data_packet_rcv.num_packets);
  

  for(i = 0; i < data_packet->num_packets; i++){
    assert(data_packet->packet_coeff[i] == data_packet_rcv.packet_coeff[i]);
  }

  assert(strcmp(data_packet->payload, data_packet_rcv.payload) == 0);

  // Print out the unmarshalled data
  printf("***Received Data***\n");
  printf("tstamp: %f\n", data_packet_rcv.tstamp);
  printf("flag: %d\n", data_packet_rcv.flag);
  printf("block length: %d\n", data_packet_rcv.blk_len);
  printf("seqno: %d\n", data_packet_rcv.seqno);
  printf("blockno: %d\n", data_packet_rcv.blockno);
  printf("numpackets: %d\n", data_packet_rcv.num_packets);
    
  for(i = 0; i < data_packet_rcv.num_packets; i++){
    printf("coding coeff %d: %d\n", i, data_packet_rcv.packet_coeff[i]);
  } 
  
  printf("payload: %s\n", data_packet_rcv.payload);


  Ack_Pckt* ack_packet = ackPacket(4, 5);
  Ack_Pckt ack_packet_rcv;
  
  memset(buf, 0, 1500);

  marshallAck(*ack_packet, buf);
  unmarshallAck(&ack_packet_rcv, buf);

  assert(ack_packet->tstamp == ack_packet_rcv.tstamp);
  assert(ack_packet->flag == ack_packet_rcv.flag);
  assert(ack_packet->ackno == ack_packet_rcv.ackno);
  assert(ack_packet->blockno == ack_packet_rcv.blockno);

  fprintf(stdout, "Passed all marshalling tests!\n");
}

void 
mult_test(uint8_t x, uint8_t y){
  printf("mult result = %d", FFmult(x,y));
}


int
main(void){
  srandom(getpid());
  marshall_test();
  //mult_test(255,1);
  return 0;
}
