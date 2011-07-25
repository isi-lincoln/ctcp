#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include "srvctcp.h"

/*
 * Handler for when the user sends the signal SIGINT by pressing Ctrl-C
 */
void
ctrlc(){
    total_time = getTime()-start_time;
    endSession();
    exit(1);
}

void
usage()
{
    fprintf(stderr, "Usage: srvctcp [-options] \n                   \
      -c    configuration file to be used ex: config/vegas\n        \
      -l    set the log name. Defaults to current datetime\n        \
      -p    port number to listen to. Defaults to 9999\n");
    exit(0);
}

int
main (int argc, char** argv){
    struct addrinfo hints, *servinfo;
    socket_t sockfd; /* network file descriptor */
    int rv;
    int c;

    srandom(getpid());

    while((c = getopt(argc,argv, "c:p:l:")) != -1)
    {
        switch (c)
        {
        case 'c':
            configfile = optarg;
            break;
        case 'p':
            port       = optarg;
            break;
        case 'l':
            log_name   = optarg;
            break;
        default:
            usage();
        }
    }

    readConfig();    // Read config file
    initialize();    // Initialize global variables and threads

    signal(SIGINT, ctrlc);

    // Setup the hints struct
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE;

    // Get the server's info
    if((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0){
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        err_sys("");
        return 1;
    }
    // Loop through all the results and connect to the first possible
    for(result = servinfo; result != NULL; result = result->ai_next) {
        if((sockfd = socket(result->ai_family,
                            result->ai_socktype,
                            result->ai_protocol)) == -1){
            perror("atousrv: error during socket initialization");
            continue;
        }
        if (bind(sockfd, result->ai_addr, result->ai_addrlen) == -1) {
            close(sockfd);
            err_sys("atousrv: can't bind local address");
            continue;
        }
        break;
    }


    if (result == NULL) { // If we are here, we failed to initialize the socket
        err_sys("atousrv: failed to initialize socket");
        return 2;
    }

    printf("Trying to bind to address %s port %d\n", inet_ntoa(((struct sockaddr_in*) &(result->ai_addr))->sin_addr), ((struct sockaddr_in*)&(result->ai_addr))->sin_port);

    //freeaddrinfo(servinfo);

    /*--------------------------------------------------------------------------*/


    char *file_name = malloc(1024);
    while(1){
      fprintf(stdout, "\nWaiting for requests...\n");
      if((numbytes = recvfrom(sockfd, file_name, 1024, 0,
                              &cli_addr, &clilen)) == -1){
        //printf("%s\n", file_name);
        err_sys("recvfrom: Failed to receive the request\n");
      }

      printf("Request for a new session: Client address %s Client port %d\n", inet_ntoa(((struct sockaddr_in*) &cli_addr)->sin_addr), ((struct sockaddr_in*)&cli_addr)->sin_port);
      printf("sending %s\n", file_name);
      
      if ((snd_file = fopen(file_name, "rb"))== NULL){
        err_sys("Error while trying to create/open a file");
      }
      
      if (debug > 3) openLog(log_name);

      // TODO TODO change restart
      restart();      
      doit(sockfd);
    }
    return 0;
}

/*
 * This is contains the main functionality and flow of the client program
 */
int
doit(socket_t sockfd){
  int i, r;

  //Substream_Path** active_paths;            // 0-1 representing whether path alive
  active_paths = malloc(MAX_CONNECT*sizeof(Substream_Path*));

  double idle_timer;

  int CurrOnFly = 0;
  //int num_active=1;                              // Connection identifier
  int path_index=0;


  Substream_Path *stream = malloc(sizeof(Substream_Path));
  init_stream(stream);
  // Save the client address as the primary client
  // cli_addr set the main loop...
  stream->cli_addr = cli_addr;
  active_paths[path_index] = stream;
  
  i=sizeof(sndbuf);

  //--------------- Setting the socket options --------------------------------//
  setsockopt(sockfd,SOL_SOCKET,SO_SNDBUF,(char *) &sndbuf,i);
  getsockopt(sockfd,SOL_SOCKET,SO_SNDBUF,(char *) &sndbuf,(socklen_t*)&i);
  setsockopt(sockfd,SOL_SOCKET,SO_RCVBUF,(char *) &rcvbuf,i);
  getsockopt(sockfd,SOL_SOCKET,SO_RCVBUF,(char *) &rcvbuf,(socklen_t*)&i);
  printf("config: sndbuf %d rcvbuf %d\n",sndbuf,rcvbuf);
  //---------------------------------------------------------------------------//

  /* send out initial segments, then go for it */
  start_time = getTime();

  // read and code the first two blocks
  for (i=1; i <= 2; i++){
    coding_job_t* job = malloc(sizeof(coding_job_t));
    job->blockno = i;
    job->dof_request = (int) ceil(BLOCK_SIZE*1.0);
    job->coding_wnd = 1; //INIT_CODING_WND;  TODO: remove comment if stable
    dof_remain[i%NUM_BLOCKS] += job->dof_request;  // Update the internal dof counter
    addJob(&workers, &coding_job, job, &free, LOW);
  }
    
  // First send_seg: CurrOnFly = 0
  send_segs(sockfd, path_index, CurrOnFly);

  Ack_Pckt *ack = malloc(sizeof(Ack_Pckt));

  while(!done){

    idle_timer = getTime();
    double rto_max = 0;
    for (i=0; i < num_active; i++){
      if (active_paths[i]->rto > rto_max) rto_max = active_paths[i]->rto;
    }
      
    r = timedread(sockfd, rto_max + RTO_BIAS);
    idle_total += getTime() - idle_timer;

    if (r > 0){  /* ack ready */
        
      // The recvfrom should be done to a separate buffer (not buff)
      r= recvfrom(sockfd, buff, ACK_SIZE, 0, &cli_addr, &clilen); 
      unmarshallAck(ack, buff);

      if (debug > 6){
        printf("Got an ACK: ackno %d blockno %d dof_req %d -- RTT est %f \n", 
               ack->ackno, 
               ack->blockno, 
               ack->dof_req, 
               getTime()-ack->tstamp);
      }

      /* ---- Decide if the ack is a request for new connections ------- */
      /* -------- Check which path the ack belong to --------------------*/
      if (ack->flag == SYN){
        if (num_active < MAX_CONNECT){

          // Compute CurrOnFly, then send. 
          CurrOnFly = countCurrOnFly(curr_block);          
          active_paths[num_active] = malloc(sizeof(Substream_Path));
          init_stream(active_paths[num_active]);
          // add the client address info to the client lookup table
          active_paths[num_active]->cli_addr = cli_addr;
          active_paths[num_active]->last_ack_time = getTime();

          num_active++;

          printf("Request for a new path: Client address %s Client port %d\n", inet_ntoa(((struct sockaddr_in*) &cli_addr)->sin_addr), ((struct sockaddr_in*)&cli_addr)->sin_port);

          // Initially send a few packets to keep it going
          send_segs(sockfd, num_active-1, CurrOnFly);
          continue;        // Go back to the beginning of the while loop
        }
      }else{
        // Use the cli_addr to find the right path_id for this Ack
        // Start searching through other possibilities
        while (sockaddr_cmp(&cli_addr, &(active_paths[path_index]->cli_addr)) != 0){
          path_index = (path_index + 1)%num_active;
        }
        //printf("path_id %d \t", path_id);
      }
      /*-----------------------------------------------------------------*/
            
      if (r <= 0) err_sys("read");

      double rcvt = getTime();
      ipkts++;
      active_paths[path_index]->last_ack_time = rcvt;

      if(handle_ack(sockfd, ack, path_index)==1){
        // Compute CurrOnFly, then send. 
        CurrOnFly = countCurrOnFly(curr_block);
        send_segs(sockfd, path_index, CurrOnFly);
      }
        
      // Check all the other paths, and see if any of them timed-out.
      for (i = 1; i < num_active; i++){
        path_index = (path_index+i)%num_active;
        if(rcvt - active_paths[path_index]->last_ack_time > active_paths[path_index]->rto + RTO_BIAS){
          if (timeout(sockfd, path_index)==TRUE){
            // Path timed out, but still alive
            // Compute CurrOnFly, then send. 
            CurrOnFly = countCurrOnFly(curr_block);
            send_segs(sockfd, path_index, CurrOnFly);
          }else{
            // Path is dead and is being removed
            removePath(path_index);
            num_active--;
            path_index--;
          }
        }
      }

    } else if (r < 0) {
      err_sys("select");
    } else if (r==0) {
      for (i = 0; i<num_active; i++){
        if (timeout(sockfd, i)==TRUE){
          // Path timed out, but still alive
          // Compute CurrOnFly, then send. 
          CurrOnFly = countCurrOnFly(curr_block);
          send_segs(sockfd, i, CurrOnFly);
        }else{
          // Path is dead and is being removed
          removePath(i);
          num_active--;
          i--;          
        }
      }
    }
    if(num_active==0){
      // no path alive, terminate
      terminate(sockfd);
    }
  }  /* while more pkts */
  total_time = getTime()-start_time;
  free(ack);
  
  terminate(sockfd); // terminate
  endSession();
  
  
  for(i=1; i<num_active; i++){
    free(active_paths[i]);
  }
  free(active_paths);
  
  return 0;
}

int
countCurrOnFly(int block){
  int block_OnFly = 0;
  int j,k;
  for (j = 0; j< num_active; j++){
    for(k = active_paths[j]->snd_una; k < active_paths[j]->snd_nxt; k++){
      block_OnFly += (active_paths[j]->OnFly[k%MAX_CWND] == block);
    }
  }
  return block_OnFly;
}

bool
minRTTPath(int index){
  double path_srtt = active_paths[index]->srtt;
  int i;
  double temp_srtt;
  for (i=0; i<num_active; i++){
    if(i==index) continue;
    temp_srtt = active_paths[i]->srtt;
    if(temp_srtt<path_srtt) return FALSE;
  }
  return TRUE;
}

void
removePath(int dead_index){
  free(active_paths[dead_index]);
  int i;
  for(i = dead_index; i<num_active; i++){
    active_paths[i] = active_paths[i+1];
  }
  active_paths[num_active-1] = NULL;
}

/*
  Returns FALSE if the path is dead
  Returns TRUE if the path is still potentially alive
 */
int
timeout(socket_t sockfd, int pin){
  Substream_Path *subpath = active_paths[pin];
        /* see if a packet has timedout */        
  if (subpath->idle > maxidle) {
    /* give up */
    printf("*** idle abort *** on path \n");
    // removing the path from connections
    return FALSE;
  }
  
  if (debug > 1){
    fprintf(stderr,
            "timerxmit %6.2f \t on %s:%d \t blockno %d blocklen %d pkt %d  snd_nxt %d  snd_cwnd %d  \n",
            getTime()-start_time,
            inet_ntoa(((struct sockaddr_in*) &subpath->cli_addr)->sin_addr),
            ((struct sockaddr_in*) &subpath->cli_addr)->sin_port,
            curr_block,
            blocks[curr_block%NUM_BLOCKS].len,
            subpath->snd_una,
            subpath->snd_nxt,
            (int)subpath->snd_cwnd);
  }

  // THIS IS A GLOBAL COUNTER FOR STATISTICS ONLY. 
  timeouts++;

  subpath->idle++;
  subpath->slr = 0;
  //slr_long[path_id] = SLR_LONG_INIT;
  subpath->rto = 2*subpath->rto; // Exponential back-off
  
  subpath->snd_ssthresh = subpath->snd_cwnd*multiplier; /* shrink */
  
  if (subpath->snd_ssthresh < initsegs) {
    subpath->snd_ssthresh = initsegs;
  }          
  subpath->slow_start = 1;
  subpath->snd_cwnd = initsegs;  /* drop window */
  subpath->snd_una = subpath->snd_nxt;
  
  return TRUE;
  // Update the path_id so that we timeout based on another path, and try every path in a round
  // Avoids getting stuck if the current path dies and packets/acks on other paths are lost
  
}

void
terminate(socket_t sockfd){
  // FIN_CLI sequence number is meaningless?
  Data_Pckt *msg = dataPacket(0, curr_block, 0);
  msg->tstamp = getTime();
  // FIN_CLI
  msg->flag = FIN_CLI;

  int size = marshallData(*msg, buff);

  do{
    // THE CLI_ADDR HERE IS SET IN THE MAIN LOOP
    if((numbytes = sendto(sockfd, buff, size, 0, &cli_addr, clilen)) == -1){
      perror("atousrv: sendto");
      exit(1);
    }

  } while(errno == ENOBUFS && ++enobufs); // use the while to increment enobufs if the condition is met

  if(numbytes != size){
    err_sys("write");
  }
  free(msg->payload);
  free(msg);
}

void
send_segs(socket_t sockfd, int pin, int CurrOnFly){

  Substream_Path* subpath = active_paths[pin];
  
  int win = 0;
  win = subpath->snd_cwnd - (subpath->snd_nxt - subpath->snd_una);

  if (win < 1) return;  /* no available window => done */

  // ** OPTIMIZING FOR MULTIPLE INTERFACE ** MINJI ** //
  bool indicator_minrtt = minRTTPath(pin);
  int CurrWin;
  int NextWin;
  int dof_needed;
  if(indicator_minrtt){
  
    CurrWin = win;
    NextWin = 0;
    
    //double p = total_loss[path_id]/snd_una[path_id];
    // Compensate for server's over estimation of the loss rate caused by lost acks
    double p = subpath->slr/(2.0-subpath->slr);   
    // The total number of dofs the we think we should be sending (for the current block) from now on
    dof_needed 
      = MAX(0,(int) (ceil((dof_req_latest 
                           + ALPHA/2*(ALPHA*p + sqrt(pow(ALPHA*p,2.0) + 4*dof_req_latest*p)))/(1-p)))- CurrOnFly);

    if (dof_req_latest - CurrOnFly < win){
      CurrWin = MIN(win, dof_needed);
      NextWin = win - CurrWin;
    }
  }else{
    dof_needed = 0;
    CurrWin = 0;
    NextWin = win;
  }
  // ** OPTIMIZING FOR MULTIPLE INTERFACE ** MINJI ** //

  // Check whether we have enough coded packets for current block
  if (dof_remain[curr_block%NUM_BLOCKS] < dof_needed){
    /*
    printf("requesting more dofs: curr path_id %d curr block %d,  dof_remain %d, dof_needed %d dof_req_latest %d\n",
           path_id, curr_block, dof_remain[curr_block%NUM_BLOCKS], dof_needed, dof_req_latest);
    */
    coding_job_t* job = malloc(sizeof(coding_job_t));
    job->blockno = curr_block;
    job->dof_request = MIN_DOF_REQUEST + dof_needed - dof_remain[curr_block%NUM_BLOCKS];
    dof_remain[curr_block%NUM_BLOCKS] += job->dof_request; // Update the internal dof counter
    job->coding_wnd = INIT_CODING_WND;
    if (dof_req_latest <= 3) {
      job->coding_wnd = MAX_CODING_WND;
      printf("Path %d, Requested jobs with coding window %d - curr blockno %d dof_needed %d  \n", 
             pin, job->coding_wnd, curr_block, dof_needed);
    }
    addJob(&workers, &coding_job, job, &free, HIGH);
  }

  while (CurrWin>=1) {
    // TODO TODO
    send_one(sockfd, curr_block, pin);
    subpath->snd_nxt++;
    CurrWin--;
    dof_remain[curr_block%NUM_BLOCKS]--;   // Update the internal dof counter
  }

  if (curr_block != maxblockno){

    // Check whether we have enough coded packets for next block
    if (dof_remain[(curr_block+1)%NUM_BLOCKS] < NextWin){

      coding_job_t* job = malloc(sizeof(coding_job_t));
      job->blockno = curr_block+1;
      job->dof_request = MIN_DOF_REQUEST + NextWin - dof_remain[(curr_block+1)%NUM_BLOCKS];
      dof_remain[(curr_block+1)%NUM_BLOCKS] += job->dof_request; // Update the internal dof counter
      job->coding_wnd = INIT_CODING_WND;
     
      addJob(&workers, &coding_job, job, &free, LOW);
    }
    // send from curr_block + 1
    while (NextWin>=1) {
      // TODO TODO
      send_one(sockfd, curr_block+1, pin);
      subpath->snd_nxt++;
      NextWin--;
      dof_remain[(curr_block+1)%NUM_BLOCKS]--;   // Update the internal dof counter
    }
  }
}


void
send_one(socket_t sockfd, uint32_t blockno, int pin){
  // Send coded packet from block number blockno
  Substream_Path *subpath = active_paths[pin];

  if (debug > 6){
    fprintf(stdout, "\n block %d DOF left %d q size %d\n",
            blockno, 
            dof_remain[blockno%NUM_BLOCKS], 
            coded_q[blockno%NUM_BLOCKS].size);
  }

  // Get a coded packet from the queue
  // q_pop is blocking. If the queue is empty, we wait until the coded packets are created
  // We should decide in send_segs whether we need more coded packets in the queue
  Data_Pckt *msg = (Data_Pckt*) q_pop(&coded_q[blockno%NUM_BLOCKS]);

  // Correct the header information of the outgoing message
  msg->seqno = subpath->snd_nxt;
  msg->tstamp = getTime();

  fprintf(db,"%f %d xmt\n", 
          getTime()-start_time, 
          blockno-curr_block);

  if (debug > 6){
    printf("Sending... on blockno %d blocklen %d  seqno %d  snd_una %d snd_nxt %d  start pkt %d snd_cwnd %d   port %d \n",
           blockno,
           blocks[curr_block%NUM_BLOCKS].len,
           msg->seqno,
           subpath->snd_una,
           subpath->snd_nxt,
           msg->start_packet,
           (int)subpath->snd_cwnd,
           ((struct sockaddr_in*)&subpath->cli_addr)->sin_port);
  }

  // Marshall msg into buf
  int message_size = marshallData(*msg, buff);

  do{
    if((numbytes = sendto(sockfd, buff, message_size, 0,
                          &subpath->cli_addr, clilen)) == -1){
      printf("Sending... on blockno %d blocklen %d  seqno %d  snd_una %d snd_nxt %d  start pkt %d snd_cwnd %d   port %d \n",
             blockno,
             blocks[curr_block%NUM_BLOCKS].len,
             msg->seqno,
             subpath->snd_una,
             subpath->snd_nxt,
             msg->start_packet,
             (int)subpath->snd_cwnd,
             ((struct sockaddr_in*)&subpath->cli_addr)->sin_port  );
      perror("atousrv: sendto");
      exit(1);
    }

  } while(errno == ENOBUFS && ++enobufs); // use the while to increment enobufs if the condition is met

    
  if(numbytes != message_size){
    err_sys("write");
  }
  // Update the packets on the fly
  subpath->OnFly[subpath->snd_nxt%MAX_CWND] = blockno;
  //printf("Freeing the message - blockno %d snd_nxt[path_id] %d ....", blockno, snd_nxt[path_id]);
  opkts++;
  free(msg->packet_coeff);
  free(msg->payload);
  free(msg);
  //printf("Done Freeing the message - blockno %d snd_nxt[path_id] %d \n\n\n", blockno, snd_nxt[path_id]);
}

void
endSession(){
  char myname[128];
  char* host = "Host"; // TODO: extract this from the packet

  gethostname(myname,sizeof(myname));
  printf("\n\n%s => %s for %f secs\n",
         myname,host, total_time);

  int i;
  for (i=0; i < num_active; i++){
    printf("******* Priniting Statistics for path %d -- %s : %d ********\n",i, 
           inet_ntoa(((struct sockaddr_in*) &(active_paths[i]->cli_addr))->sin_addr),
           ((struct sockaddr_in*)&(active_paths[i]->cli_addr))->sin_port);
    printf("**THRU** %f Mbs\n",
           8.e-6*(active_paths[i]->snd_una*PAYLOAD_SIZE)/total_time);
    printf("**LOSS* %6.3f%% \n",
           100.*active_paths[i]->total_loss/active_paths[i]->snd_una);
    if (ipkts) active_paths[i]->avrgrtt /= ipkts;
    printf("**RTT** minrtt  %f maxrtt %f avrgrtt %f\n",
           active_paths[i]->minrtt, active_paths[i]->maxrtt,active_paths[i]->avrgrtt);
    printf("**RTT** rto %f  srtt %f \n", active_paths[i]->rto, active_paths[i]->srtt);
    printf("**VEGAS** max_delta %f vdecr %d v0 %d vdelta %f\n", 
           active_paths[i]->max_delta ,active_paths[i]->vdecr, active_paths[i]->v0,active_paths[i]->vdelta);
    printf("**CWND** snd_nxt %d snd_cwnd %5.3f  snd_una %d ssthresh %d goodacks %d\n\n",
           active_paths[i]->snd_nxt, active_paths[i]->snd_cwnd, active_paths[i]->snd_una, 
           active_paths[i]->snd_ssthresh, goodacks);
  }

  printf("Total idle time %f, Total timeouts %d\n", idle_total, timeouts);
  printf("Total packets in: %d, out: %d, enobufs %d\n", ipkts, opkts, enobufs);

  if(snd_file) fclose(snd_file);
  if(db)       fclose(db);

  snd_file = NULL;
  db       = NULL;
}

/*
  Returns 1 if subpath sp ready to send more
  Returns 0 if subpath sp is not ready to send (bad ack or done)
 */
int
handle_ack(socket_t sockfd, Ack_Pckt *ack, int pin){
  Substream_Path *subpath = active_paths[pin];

  uint32_t ackno = ack->ackno;

  if (debug > 8 )printf("ack rcvd %d\n", ackno);

  //------------- RTT calculations --------------------------//
  double rtt;
  rtt = subpath->last_ack_time - ack->tstamp; // this calculates the rtt for this coded packet
  if (rtt < subpath->minrtt) subpath->minrtt = rtt;
  if (rtt > subpath->maxrtt) subpath->maxrtt = rtt;
  subpath->avrgrtt += rtt;
  subpath->srtt = (1-g)*subpath->srtt + g*rtt; 
  if (rtt > subpath->rto/beta){
    subpath->rto = (1-g)*subpath->rto + g*beta*rtt;
  }else {
    subpath->rto = (1-g/5)*subpath->rto + g/5*beta*rtt;
  }
  if (ackno > subpath->snd_una){
    subpath->vdelta = 1-subpath->srtt/rtt;
    // max_delta: only for statistics
    if (subpath->vdelta > subpath->max_delta) subpath->max_delta = subpath->vdelta;  /* vegas delta */
  }
  if (debug > 6) {
    fprintf(db,"%f %d %f  %d %d ack\n",
            subpath->last_ack_time - start_time,
            ackno,
            rtt,
            (int)subpath->snd_cwnd,
            subpath->snd_ssthresh);
  }
  //------------- RTT calculations --------------------------//


  if (ack->blockno > curr_block){
    // Moving on to a new block

    pthread_mutex_lock(&blocks[curr_block%NUM_BLOCKS].block_mutex);

    freeBlock(curr_block);
    q_free(&coded_q[curr_block%NUM_BLOCKS], &free_coded_pkt);
    curr_block++;                      // Update the current block identifier

    pthread_mutex_unlock(&blocks[(curr_block-1)%NUM_BLOCKS].block_mutex);


    if(maxblockno && ack->blockno > maxblockno){
      done = TRUE;
      printf("THIS IS THE LAST ACK\n");
      return 0; // goes back to the beginning of the while loop in main() and exits
    }

    if (!maxblockno){

      coding_job_t* job = malloc(sizeof(coding_job_t));
      job->blockno = curr_block+1;
      job->dof_request = BLOCK_SIZE;
      dof_remain[(curr_block+1)%NUM_BLOCKS] += job->dof_request;  // Update the internal dof counter
      job->coding_wnd = 1;

      addJob(&workers, &coding_job, job, &free, LOW);
    }

    dof_req_latest = ack->dof_req;     // reset the dof counter for the current block

    if (debug > 5 && curr_block%10==0){
      printf("Now sending block %d, cwnd %f, SLR %f%%, SRTT %f ms \n", 
             curr_block, subpath->snd_cwnd, 100*subpath->slr, subpath->srtt*1000);
    }
  }

  if (ackno > subpath->snd_nxt || ack->blockno != curr_block) {
    /* bad ack */
    if (debug > 4) fprintf(stderr,
                           "Bad ack path %d: curr block %d ack blockno %d badack no %d snd_nxt %d snd_una %d cli.port %d, cli_storage[path_id].port %d\n\n",                            
                           pin,
                           curr_block, 
                           ack->blockno,
                           ackno, 
                           subpath->snd_nxt, 
                           subpath->snd_una,  
                           ((struct sockaddr_in*)&cli_addr)->sin_port,  
                           ((struct sockaddr_in*)&subpath->cli_addr)->sin_port);

    badacks++;
    if(subpath->snd_una < ackno) subpath->snd_una = ackno;

  } else {
    // Late or Good acks count towards goodput

    fprintf(db,"%f %d %f %d %f %f %f %f %f rcv\n", 
            getTime()-start_time, 
            ack->blockno, 
            subpath->snd_cwnd, 
            subpath->snd_ssthresh, 
            subpath->slr, 
            subpath->slr_long, 
            subpath->srtt, 
            subpath->rto, 
            rtt);

    subpath->idle = 0; // Late or good acks should stop the "idle" count for max-idle abort.
          
    if (ackno <= subpath->snd_una){
      //late ack
      if (debug > 5) fprintf(stderr,
                             "Late ack path %d: curr block %d ack-blockno %d badack no %d snd_nxt %d snd_una %d\n",
                             pin, curr_block, ack->blockno, ackno, subpath->snd_nxt, subpath->snd_una);
    } else {
      goodacks++;
      int losses = ackno - (subpath->snd_una +1);
      /*
        if (losses > 0){
        printf("Loss report curr block %d ackno - snd_una[path_id] %d\n", curr_block, ackno - snd_una[path_id]);
        }
      */
      subpath->total_loss += losses;

      double loss_tmp =  pow(1-slr_mem, losses);
      subpath->slr = loss_tmp*(1-slr_mem)*subpath->slr + (1 - loss_tmp);
      loss_tmp =  pow(1-slr_longmem, losses);
      subpath->slr_long = loss_tmp*(1-slr_longmem)*subpath->slr_long + (1 - loss_tmp);

      // NECESSARY CONDITION: slr_longmem must be smaller than 1/2. 
      subpath->slr_longstd = (1-slr_longmem)*subpath->slr_longstd 
        + slr_longmem*(fabs(subpath->slr - subpath->slr_long) - subpath->slr_longstd);
      subpath->snd_una = ackno;
    }
    // Updated the requested dofs for the current block 
    // The MIN is to avoid outdated infromation by out of order ACKs or ACKs on different paths
    dof_req_latest = MIN(dof_req_latest, ack->dof_req);

    advance_cwnd(pin);
    /*subpath->snd_cwnd = advance_cwnd(subpath->snd_cwnd, 
                                     subpath->snd_ssthresh, 
                                     subpath->slow_start, 
                                     subpath->vdelta, 
                                     subpath->slr, 
                                     subpath->slr_long, 
                                     subpath->slr_longstd);
                                     if(subpath->snd_cwnd > subpath->snd_ssthresh) subpath->slow_start = 0;*/
    return 1;
  } // end else goodack
  return 0;
}

// Perhaps this is unnecesary.... No need to use select -> maybe use libevent (maybe does not have timeou?)
socket_t
timedread(socket_t sockfd, double t){
  struct timeval tv;
  fd_set rset;

  tv.tv_sec = t;
  tv.tv_usec = (t - tv.tv_sec)*1000000;
  FD_ZERO(&rset);
  FD_SET(sockfd, &rset);
  return ( select(sockfd+1,&rset,NULL,NULL, &tv) );
}

void
err_sys(char* s){
  perror(s);
  endSession();
  exit(1);
}

void
readConfig(void){
  // Initialize the default values of config variables
  
  rcvrwin    = 20;          /* rcvr window in mss-segments */
  increment  = 1;           /* cc increment */
  multiplier = 0.5;         /* cc backoff  &  fraction of rcvwind for initial ssthresh*/
  initsegs   = 2;          /* slowstart initial */
  ssincr     = 1;           /* slow start increment */
  maxpkts    = 0;           /* test duration */
  maxidle    = 10;          /* max idle before abort */
  valpha     = 0.05;        /* vegas parameter */
  vbeta      = 0.2;         /* vegas parameter */
  sndbuf     = MSS*MAX_CWND;/* UDP send buff, bigger than mss */
  rcvbuf     = MSS*MAX_CWND;/* UDP recv buff for ACKs*/
  debug      = 5           ;/* Debug level */

  /* read config if there, keyword value */
  FILE *fp;
  char line[128], var[32];
  double val;
  time_t t;

  fp = fopen(configfile,"r");

  if (fp == NULL) {
    printf("ctcp unable to open %s\n",configfile);
    return;
  }

  while (fgets(line, sizeof (line), fp) != NULL) {
    sscanf(line,"%s %lf",var,&val);
    if (*var == '#') continue;
    else if (strcmp(var,"rcvrwin")==0) rcvrwin = val;
    else if (strcmp(var,"increment")==0) increment = val;
    else if (strcmp(var,"multiplier")==0) multiplier = val;
    else if (strcmp(var,"initsegs")==0) initsegs = val;
    else if (strcmp(var,"ssincr")==0) ssincr = val;
    else if (strcmp(var,"maxpkts")==0) maxpkts = val;
    else if (strcmp(var,"maxidle")==0) maxidle = val;
    else if (strcmp(var,"valpha")==0) valpha = val;
    else if (strcmp(var,"vbeta")==0) vbeta = val;
    else if (strcmp(var,"sndbuf")==0) sndbuf = val;
    else if (strcmp(var,"rcvbuf")==0) rcvbuf = val;
    else if (strcmp(var,"debug")==0) debug = val;
    else printf("config unknown: %s\n",line);
  }

  t = time(NULL);
  printf("*** CTCP %s ***\n",version);
  printf("config: port %s debug %d, %s",port,debug, ctime(&t));
  printf("config: rcvrwin %d  increment %d  multiplier %f\n",
         rcvrwin,increment,multiplier);
  printf("config: alpha %f beta %f\n", valpha,vbeta);

}


void
advance_cwnd(int pin){
  /* advance cwnd according to slow-start of congestion avoidance */
  Substream_Path *subpath = active_paths[pin];
  if (subpath->snd_cwnd <= subpath->snd_ssthresh && subpath->slow_start) {
    /* slow start, expo growth */
    subpath->snd_cwnd = subpath->snd_cwnd+ssincr;
    return;
  } else{
    /* congestion avoidance phase */
    int incr;
    incr = increment;
    /*
      Range --(1)-- valpha --(2)-- vbeta --(3)--
      (1): increase window
      (2): stay
      (3): decrease window
    */
    if (subpath->vdelta > vbeta ){
      if (debug > 6){
        printf("vdelta %f going down from %f \n", subpath->vdelta, subpath->snd_cwnd);
      }
      incr= -increment; /* too fast, -incr /RTT */
      subpath->vdecr++;
    } else if (subpath->vdelta > valpha) {
      if (debug > 6){
        printf("vdelta %f staying at %f\n", subpath->vdelta, subpath->snd_cwnd);
      }
      incr =0; /* just right */
      subpath->v0++;
    }
    subpath->snd_cwnd += incr/subpath->snd_cwnd; /* ca */
    subpath->slow_start = 0;
  }
  if (subpath->slr > subpath->slr_long + subpath->slr_longstd){
    subpath->snd_cwnd -= subpath->slr/2;
  }
  if (subpath->snd_cwnd < initsegs) subpath->snd_cwnd = initsegs;
  if (subpath->snd_cwnd > MAX_CWND) subpath->snd_cwnd = MAX_CWND;
  return;
}

//---------------WORKER FUNCTION ----------------------------------
void*
coding_job(void *a){
  coding_job_t* job = (coding_job_t*) a;
  //printf("Job processed by thread %lu: blockno %d dof %d\n", pthread_self(), job->blockno, job->dof_request);

  uint32_t blockno = job->blockno;
  int dof_request = job->dof_request;
  int coding_wnd = job->coding_wnd;

  pthread_mutex_lock(&blocks[blockno%NUM_BLOCKS].block_mutex);
    
  // Check if the blockno is already done
    if( blockno < curr_block ){
      if (debug > 5){
        printf("Coding job request for old block - curr_block %d blockno %d dof_request %d \n\n", curr_block,  blockno, dof_request);
      }
      goto release;
    }


  // Check whether the requested blockno is already read, if not, read it from the file
  // generate the first set of degrees of freedom according toa  random permutation

  uint8_t block_len = blocks[blockno%NUM_BLOCKS].len;

  if (block_len  == 0){
    if( !maxblockno || maxblockno >= blockno )
      {
        // lock the file
        pthread_mutex_lock(&file_mutex);

        readBlock(blockno);

        // unlock the file
        pthread_mutex_unlock(&file_mutex);
      }else{
      goto release;
    }

    if( (block_len = blocks[blockno%NUM_BLOCKS].len) == 0){
      goto release;
    }
    // Compute a random permutation of the rows

    uint8_t order[block_len];
    int i, j, swap_temp;
    for (i=0; i < block_len; i++){
      order[i] = i;
    }
    for (i=block_len - 1; i > 0; i--){
      j = random()%(i+1);
      swap_temp = order[i];
      order[i] = order[j];
      order[j] = swap_temp;
    }

    // Make sure this never happens!
    if (dof_request < block_len){
      printf("Error: the initially requested dofs are less than the block length - blockno %d dof_request %d block_len %d\n\n\n\n\n",  blockno, dof_request, block_len);
      dof_request = block_len;
    }


    // Generate random combination by picking rows based on order
    int dof_ix, row;
    for (dof_ix = 0; dof_ix < block_len; dof_ix++){
      uint8_t num_packets = MIN(coding_wnd, block_len);
      Data_Pckt *msg = dataPacket(0, blockno, num_packets);

      row = order[dof_ix];

      // TODO Fix this, i.e., make sure every packet is involved in coding_wnd equations
      msg->start_packet = MIN(MAX(row%block_len - (coding_wnd-1)/2, 0), MAX(block_len - coding_wnd, 0));
      //memset(msg->payload, 0, PAYLOAD_SIZE);
      msg->packet_coeff[0] = 1;
      memcpy(msg->payload, blocks[blockno%NUM_BLOCKS].content[msg->start_packet], PAYLOAD_SIZE);

      for(i = 1; i < num_packets; i++){
        msg->packet_coeff[i] = (uint8_t)(1 + random()%255);
        for(j = 0; j < PAYLOAD_SIZE; j++){
          msg->payload[j] ^= FFmult(msg->packet_coeff[i], blocks[blockno%NUM_BLOCKS].content[msg->start_packet+i][j]);
        }
      }
      if(block_len < BLOCK_SIZE){
        msg->flag = PARTIAL_BLK;
        msg->blk_len = block_len;
      }
      /*
        printf("Pushing ... block %d, row %d \t start pkt %d\n", blockno, row, msg->start_packet);
        fprintf(stdout, "before BEFORE push  queue size %d HEAD %d, TAIL %d\n",coded_q[blockno%NUM_BLOCKS].size, coded_q[blockno%NUM_BLOCKS].head, coded_q[blockno%NUM_BLOCKS].tail);

        if (coded_q[blockno%NUM_BLOCKS].size > 0){
        int k;
        for (k=1; k <= coded_q[blockno%NUM_BLOCKS].size; k++){
        Data_Pckt *tmp = (Data_Pckt*) coded_q[blockno%NUM_BLOCKS].q_[coded_q[blockno%NUM_BLOCKS].tail+k];
        printf("before BEFORE push buff msg block no %d start pkt %d\n", tmp->blockno, tmp->start_packet);
        }
        }
      */
      q_push_back(&coded_q[blockno%NUM_BLOCKS], msg);
    }  // Done with forming the initial set of coded packets
    dof_request = MAX(0, dof_request - block_len);  // This many more to go
  }

  if (dof_request > 0){
    // Extra degrees of freedom are generated by picking a row randomly

    int i, j;
    int dof_ix, row;

    int coding_wnd_slope = floor((MAX_CODING_WND - coding_wnd)/dof_request);

    for (dof_ix = 0; dof_ix < dof_request; dof_ix++){

      coding_wnd += coding_wnd_slope;

      uint8_t num_packets = MIN(coding_wnd, block_len);
      int partition_size = ceil(block_len/num_packets);
      Data_Pckt *msg = dataPacket(0, blockno, num_packets);

      row = (random()%partition_size)*num_packets;
      // TODO Fix this, i.e., make sure every packet is involved in coding_wnd equations
      msg->start_packet = MIN(row, block_len - num_packets);

      //printf("selected row: %d, start packet %d \n", row, msg->start_packet);

      memset(msg->payload, 0, PAYLOAD_SIZE);

      msg->packet_coeff[0] = 1;
      memcpy(msg->payload, blocks[blockno%NUM_BLOCKS].content[msg->start_packet], PAYLOAD_SIZE);

      for(i = 1; i < num_packets; i++){
        msg->packet_coeff[i] = (uint8_t)(1 + random()%255);

        for(j = 0; j < PAYLOAD_SIZE; j++){
          msg->payload[j] ^= FFmult(msg->packet_coeff[i], blocks[blockno%NUM_BLOCKS].content[msg->start_packet+i][j]);
        }
      }

      if(block_len < BLOCK_SIZE){
        msg->flag = PARTIAL_BLK;
        msg->blk_len = block_len;
      }
      q_push_back(&coded_q[blockno%NUM_BLOCKS], msg);
    }  // Done with forming the remaining set of coded packets

  }
  //printf("Almost done with block %d\n", blockno);

 release:

  pthread_mutex_unlock( &blocks[blockno%NUM_BLOCKS].block_mutex );

  return NULL;
}

//----------------END WORKER ---------------------------------------

// Free Handler for the coded packets in coded_q
void
free_coded_pkt(void* a)
{
  Data_Pckt* msg = (Data_Pckt*) a;
  //printf("freeing msg blockno %d start pkt %d\n", msg->blockno, msg->start_packet);
  free(msg->packet_coeff);
  free(msg->payload);
  free(msg);
}

//--------------------------------------------------------------------
void
readBlock(uint32_t blockno){

  // TODO: Make sure that the memory in the block is released before calling this function
  blocks[blockno%NUM_BLOCKS].len = 0;
  blocks[blockno%NUM_BLOCKS].content = malloc(BLOCK_SIZE*sizeof(char*));

  if (file_position != blockno){
    fseek(snd_file, (blockno-1)*BLOCK_SIZE*(PAYLOAD_SIZE-2), SEEK_SET);
  }

  while(blocks[blockno%NUM_BLOCKS].len < BLOCK_SIZE && !feof(snd_file)){
    char* tmp = malloc(PAYLOAD_SIZE);
    memset(tmp, 0, PAYLOAD_SIZE); // This is done to pad with 0's
    uint16_t bytes_read = (uint16_t) fread(tmp + 2, 1, PAYLOAD_SIZE-2, snd_file);
    bytes_read = htons(bytes_read);
    memcpy(tmp, &bytes_read, sizeof(uint16_t));

    // Insert this pointer into the blocks datastructure
    blocks[blockno%NUM_BLOCKS].content[blocks[blockno%NUM_BLOCKS].len] = tmp;
    blocks[blockno%NUM_BLOCKS].len++;
    if(feof(snd_file)){
      maxblockno = blockno;
      printf("This is the last block %d\n", maxblockno);
    }    
  }

  file_position = blockno + 1;  // Advance the counter


}


/*
 * Frees a block from memory
 */
void
freeBlock(uint32_t blockno){
  int i;
  for(i = 0; i < blocks[blockno%NUM_BLOCKS].len; i++){
    free(blocks[blockno%NUM_BLOCKS].content[i]);
  }
  free(blocks[blockno%NUM_BLOCKS].content);
  // reset the counters
  blocks[blockno%NUM_BLOCKS].len = 0;
  dof_remain[blockno%NUM_BLOCKS] = 0;
}

void
openLog(char* log_name){

  char* file;
  time_t rawtime;
  struct tm* ptm;
  time(&rawtime);
  ptm = localtime(&rawtime);


  //---------- Remake Log and Fig Directories ----------------//

  if(!mkdir("logs", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)){
    perror("An error occurred while making the logs directory");
  }

  if(!mkdir("figs", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)){
    perror("An error occurred while making the figs directory");
  }

  char* dir_name = malloc(20);

  sprintf(dir_name, "figs/%d-%02d-%02d",
          ptm->tm_year + 1900,
          ptm->tm_mon + 1,
          ptm->tm_mday);

  if(!mkdir(dir_name, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)){
    perror("An error occurred while making the fig date directory");
  }

  sprintf(dir_name, "logs/%d-%02d-%02d",
          ptm->tm_year + 1900,
          ptm->tm_mon + 1,
          ptm->tm_mday);

  if(!mkdir(dir_name, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)){
    perror("An error occurred while making the log date directory");
  }

  //------------------------------------------------//

  int auto_log = !log_name;

  if(auto_log)
    {
      file = malloc(15);
      log_name = malloc(32);

      sprintf(file, "%02d:%02d.%02d.log",
              ptm->tm_hour,
              ptm->tm_min,
              ptm->tm_sec);

      sprintf(log_name, "%s/%s",
              dir_name,
              file );
    }

  db = fopen(log_name, "w+");

  if(!db){
    perror("An error ocurred while opening the log file");
  }
    

  if(auto_log)
    {
      free(file);
      free(dir_name);
      free(log_name);
    }
}

/*
 * Takes a Data_Pckt struct and puts its raw contents into the buffer.
 * This assumes that there is enough space in buf to store all of these.
 * The return value is the number of bytes used for the marshalling
 */
int
marshallData(Data_Pckt msg, char* buf){
  int index = 0;
  int part = 0;

  int partial_blk_flg = 0;
  if (msg.flag == PARTIAL_BLK) partial_blk_flg = sizeof(msg.blk_len);

  // the total size in bytes of the current packet
  int size = PAYLOAD_SIZE 
    + sizeof(double) 
    + sizeof(flag_t) 
    + sizeof(msg.seqno) 
    + sizeof(msg.blockno) 
    + (partial_blk_flg) 
    + sizeof(msg.start_packet) 
    + sizeof(msg.num_packets) 
    + msg.num_packets*sizeof(msg.packet_coeff); 

  //Set to zeroes before starting
  memset(buf, 0, size);

  // Marshall the fields of the packet into the buffer

  htonpData(&msg);
  memcpy(buf + index, &msg.tstamp, (part = sizeof(msg.tstamp)));
  index += part;
  memcpy(buf + index, &msg.flag, (part = sizeof(msg.flag)));
  index += part;
  memcpy(buf + index, &msg.seqno, (part = sizeof(msg.seqno)));
  index += part;
  memcpy(buf + index, &msg.blockno, (part = sizeof(msg.blockno)));
  index += part;

  if (partial_blk_flg > 0){
    memcpy(buf + index, &msg.blk_len, (part = sizeof(msg.blk_len)));
    index += part;
  }
  memcpy(buf + index, &msg.start_packet, (part = sizeof(msg.start_packet)));
  index += part;

  memcpy(buf + index, &msg.num_packets, (part = sizeof(msg.num_packets)));
  index += part;

  int i;
  for(i = 0; i < msg.num_packets; i ++){
    memcpy(buf + index, &msg.packet_coeff[i], (part = sizeof(msg.packet_coeff[i])));
    index += part;
  }

  memcpy(buf + index, msg.payload, (part = PAYLOAD_SIZE));
  index += part;

  /*
  //----------- MD5 Checksum calculation ---------//
  MD5_CTX mdContext;
  MD5Init(&mdContext);
  MD5Update(&mdContext, buf, size);
  MD5Final(&mdContext);

  // Put the checksum in the marshalled buffer
  int i;
  for(i = 0; i < CHECKSUM_SIZE; i++){
  memcpy(buf + index, &mdContext.digest[i], (part = sizeof(mdContext.digest[i])));
  index += part;
  }*/

  return index;
}

bool
unmarshallAck(Ack_Pckt* msg, char* buf){
  int index = 0;
  int part = 0;

  memcpy(&msg->tstamp, buf+index, (part = sizeof(msg->tstamp)));
  index += part;
  memcpy(&msg->flag, buf+index, (part = sizeof(msg->flag)));
  index += part;
  memcpy(&msg->ackno, buf+index, (part = sizeof(msg->ackno)));
  index += part;
  memcpy(&msg->blockno, buf+index, (part = sizeof(msg->blockno)));
  index += part;
  memcpy(&msg->dof_req, buf+index, (part = sizeof(msg->dof_req)));
  index += part;
  ntohpAck(msg);

  bool match = TRUE;
  /*
    int begin_checksum = index;

    // -------------------- Extract the MD5 Checksum --------------------//
    int i;
    for(i=0; i < CHECKSUM_SIZE; i++){
    memcpy(&msg->checksum[i], buf+index, (part = sizeof(msg->checksum[i])));
    index += part;
    }

    // Before computing the checksum, fill zeroes where the checksum was
    memset(buf+begin_checksum, 0, CHECKSUM_SIZE);

    //-------------------- MD5 Checksum Calculation  -------------------//
    MD5_CTX mdContext;
    MD5Init(&mdContext);
    MD5Update(&mdContext, buf, msg->payload_size + HDR_SIZE);
    MD5Final(&mdContext);


    for(i = 0; i < CHECKSUM_SIZE; i++){
    if(msg->checksum[i] != mdContext.digest[i]){
    match = FALSE;
    }
    }*/
  return match;
}


// Compare the IP address and Port of two sockaddr structs

int 
sockaddr_cmp(struct sockaddr* addr1, struct sockaddr* addr2){

  if (addr1->sa_family != addr2->sa_family)    return 1;   // No match
  
  if (addr1->sa_family == AF_INET){
    // IPv4 format
    // Cast to the IPv4 struct
    struct sockaddr_in *tmp1 = (struct sockaddr_in*)addr1;
    struct sockaddr_in *tmp2 = (struct sockaddr_in*)addr2;
  
    if (tmp1->sin_port != tmp2->sin_port) return 1;                // ports don't match
    if (tmp1->sin_addr.s_addr != tmp2->sin_addr.s_addr) return 1;  // Addresses don't match
    
    return 0; // We have a match
  } else if (addr1->sa_family == AF_INET6){
    // IPv6 format
    // Cast to the IPv6 struct
    struct sockaddr_in6 *tmp1 = (struct sockaddr_in6*)addr1;
    struct sockaddr_in6 *tmp2 = (struct sockaddr_in6*)addr2;
  
    if (tmp1->sin6_port != tmp2->sin6_port) return 1;                // ports don't match
    if (memcmp(&tmp1->sin6_addr, &tmp2->sin6_addr, sizeof(struct in6_addr)) != 0) return 1;  // Addresses don't match
    
    return 0; // We have a match
    
  } else {
    printf("Cannot recognize socket address family\n");
    return 1;
  }
 
}


// Initialize the global objects (called only once)
void
initialize(void){
  int i;

  // initialize the thread pool
  thrpool_init( &workers, THREADS );
  
  // Initialize the file mutex and position
  pthread_mutex_init(&file_mutex, NULL);
    
  // Initialize the block mutexes and queue of coded packets and counters
  for(i = 0; i < NUM_BLOCKS; i++){
    pthread_mutex_init( &blocks[i].block_mutex, NULL );
    q_init(&coded_q[i], 2*BLOCK_SIZE);
  }

}

void
init_stream(Substream_Path *subpath){
  subpath->dof_req = BLOCK_SIZE;
  
  int j;
  for(j=0; j < MAX_CWND; j++) subpath->OnFly[j] = 0;
  
  subpath->last_ack_time = 0;
  subpath->snd_nxt = 1;
  subpath->snd_una = 1;
  subpath->snd_cwnd = initsegs;
  
  if (multiplier) {
    subpath->snd_ssthresh = multiplier*MAX_CWND;
  } else {
    subpath->snd_ssthresh = 2147483647;  /* normal TCP, infinite */
  }
    // Statistics // 
  subpath->idle       = 0;
  subpath->vdelta     = 0;    /* Vegas delta */
  subpath->max_delta  = 0;
  subpath->slow_start = 1;    /* in vegas slow start */
  subpath->vdecr      = 0;
  subpath->v0         = 0;    /* vegas decrements or no adjusts */

  
  subpath->minrtt     = 999999.0;
  subpath->maxrtt     = 0;
  subpath->avrgrtt    = 0;
  subpath->srtt       = 0;
  subpath->rto        = INIT_RTO;
  
  subpath->slr        = 0;
  subpath->slr_long   = SLR_LONG_INIT;
  subpath->slr_longstd= 0;
  subpath->total_loss = 0;
  
  //subpath->cli_addr   = NULL;
}

// Initialize all of the global variables except the ones read from config file
void
restart(void){
  // Print to the db file to differentiate traces
  fprintf(stdout, "\n\n*************************************\n****** Starting New Connection ******\n*************************************\n");



  int i;

  for(i = 0; i < NUM_BLOCKS; i++){
    dof_remain[i] = 0;
  }

  memset(buff,0,BUFFSIZE);        /* pretouch */

  dof_req_latest = BLOCK_SIZE;
  //num_active = 0;

  //--------------------------------------------------------
  done = FALSE;
  curr_block = 1;
  maxblockno = 0;
  file_position = 1; // Initially called to read the first block

  timeouts   = 0;
  ipkts      = 0;
  opkts      = 0;
  badacks    = 0;
  goodacks   = 0;
  enobufs    = 0;
  start_time = 0;
  idle_total = 0;
}



