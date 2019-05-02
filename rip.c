/*
Use gcc rip.c -o rip -lpthread to compile

Author: Ran Bi

*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h> 
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <signal.h>
#include <sys/select.h>
#include <stdarg.h>
#include <fcntl.h>

#define BUF_SIZE 1500
#define MAX_HOP 15
#define UPDATE 5
#define TIMEOUT UPDATE*6
#define GARBAGE UPDATE*4
#define COMMAND 1
#define VERSION 2
#define ADDRFAMILY 2

typedef char byte;


char *configName[] =  {"router-id", "input-ports", "outputs"};

//============== Global Variables ============================

struct Peer
{
    int port;
    int metric;
    int routerid;
};

struct Interface
{
    int port;
    int sockfd;
    struct Peer *neighbor;
    bool found_peer;
};

struct ConfigItem
{
    int routerid;
    int input_number;
    int output_number;
    bool routerid_status;
    struct Interface *input;
    struct Peer *output;
}self;

struct Timer_Struct
{
    struct timeval timer;
    void (*fun_ptr)();
    void *args;
    pthread_t timer_thread;    //the PID of timer threads which we can easily manage the timer
    bool valid;      //if receive good msg while invalid, cancel the garbage handler and re-init the timer
};

struct Route_Table
{   
    struct Route_Table *next;    //linked-list with structure node
    int address;                 //dest address
    uint32_t metric;
    int next_hop;
    bool flag;                  //mark to trigger update
    struct Timer_Struct timeout;
    struct Timer_Struct garbage;
    int iface;                  //the interface to next hop
                         
    
    //struct Timer_Struct timerdata;
}*routetable;

pthread_mutex_t write_route_table;




struct ripHeader {
  byte command; 		// 1-REQUEST, 2-RESPONSE 
  byte version;
  short int routerid;       // it should be zero, but we used as router-id
};


struct ripEntry {
  short int addrfamily;
  short int zero;
  uint32_t destination;    // neighbor port
  uint32_t zero1;
  uint32_t zero2;
  uint32_t metric;
};

struct packet
{
    char *message;
    int size;
};



//=============================================================

void exit_program()
{
    free(self.input);
    free(self.output);

    exit(0);
}

void log_handler(const char *fmt, ...)
{

    time_t t;
    time(&t);
    struct tm *tmp_time = localtime(&t);
    char s[100];
    strftime(s, sizeof(s), "%04Y%02m%02d-%H:%M:%S", tmp_time);
    printf("[LOG-%s]: ", s);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void* time_handler(void *args)
{
    struct Timer_Struct *timerdata = (struct Timer_Struct *)args;  //formating the passing args
    log_handler("Starting Timer, %d\n", timerdata->timer.tv_sec);       //debug
    time_t nowtime, starttime;        //set time stamp

    
          //remember start time
    while (timerdata->timer.tv_sec > 0 && timerdata->valid)
    {
        //log_handler("Time left %d\n", timerdata->timer->tv_sec);

        time(&starttime); 
        sleep(1);         //wait 1 second
        time(&nowtime);   //check current time
        if (timerdata->timer.tv_sec > nowtime - starttime)
            timerdata->timer.tv_sec -= nowtime - starttime;
        else timerdata->timer.tv_sec = 0;
        //log_handler("time still left:%d \n", timerdata->timer->tv_sec);
    }

    if (timerdata->valid)  (timerdata->fun_ptr)(timerdata->args);       //call the function
    
    pthread_exit(0);                      //exit thread

    
}

//void set_time(struct timeval *timer, void (*fun_ptr)(), void *args)
void set_time(struct Timer_Struct *timerdata, pthread_t *timer_thread)
{
    //pthread_t timer_thread;          //create timer thread id
    /*struct Timer_Struct timerdata;    //initial passing args
    timerdata.timer = *timer;
    timerdata.fun_ptr = fun_ptr;
    timerdata.args = args;*/


    log_handler("Set Timer %d\n", timerdata->timer.tv_sec); 
    if (pthread_create(timer_thread,NULL,time_handler,timerdata) != 0) //create timer thread
    {
            perror("TimeHandler");
            exit(1);
    }

    /*if (pthread_join(timer_thread, NULL) != 0) {       //waiting for timer thread quit
            perror("pthread_join");
            exit(1);
        }*/
    log_handler("Timer quit success\n");

}


void packet_header(struct packet *msg, struct ripHeader rh)
{   

    msg->size += sizeof(rh);
    msg->message = (char*)malloc(msg->size);
    memset(msg->message, '\0', msg->size);
    memcpy(msg->message, (void*)&rh, sizeof(rh));
}

void packet_entry(struct packet *msg, struct ripEntry re)
{   

    
    msg->message = (char*)realloc(msg->message, msg->size + sizeof(re));
    memset(msg->message + msg->size, '\0', sizeof(re));
    memcpy(msg->message + msg->size, (void*)&re, sizeof(re));
    msg->size += sizeof(re);
}



void generate_update(struct packet *msg, int nexthop, struct Route_Table *node) 
{ 
    struct ripHeader rh;
    struct ripEntry re;
    rh.command = COMMAND;
    rh.version = VERSION;
    rh.routerid = self.routerid;
    packet_header(msg, rh);

    re.addrfamily = ADDRFAMILY;
    re.zero = 0;
    re.zero1 = 0;
    re.zero2 = 0;
    /*re.destination = self.routerid;
    re.metric = 0;
    packet_entry(msg, re);*/

    if (node == NULL)  //regular update
    {
        struct Route_Table *item = routetable;
        while(item != NULL)
        {
            if (item->next_hop != nexthop) re.metric = item->metric;
            else re.metric = MAX_HOP + 1;    //poison reverse
            re.destination = item->address;
            packet_entry(msg, re);

            item = item->next;
        }
    }
    else   //triggered update
    {
        if (node->next_hop != nexthop) re.metric = node->metric;
        else re.metric = MAX_HOP + 1;    //poison reverse
        re.destination = node->address;
        packet_entry(msg, re);
    }

} 



void send_update(struct Interface *interface, struct Route_Table *node)
{
    int rc;
    struct sockaddr_in remote; 


    remote.sin_family = AF_INET;          /* communicate using internet address */
    remote.sin_addr.s_addr = INADDR_ANY; /* accept all calls                   */
    
    
    struct packet msg;
    msg.size = 0;

    if (interface->found_peer) 
    {
        remote.sin_port = htons(interface->neighbor->port);    /* this is port number  */
        generate_update(&msg, interface->neighbor->routerid, node) ;
        rc = sendto(interface->sockfd, msg.message, msg.size, 0, (struct sockaddr *)&remote, sizeof(remote));
        //int rc = sendto(interface->sockfd, "update", 6, 0, (struct sockaddr *)&remote, sizeof(remote));
        if (rc == -1) {
            perror("sendto error");
        }
    }
    else 
    {

        generate_update(&msg, -1, node) ;
        for(int i = 0; i < self.output_number; i++)
        {
            remote.sin_port = htons(self.output[i].port);    /* this is port number  */
            rc = sendto(interface->sockfd, msg.message, msg.size, 0, (struct sockaddr *)&remote, sizeof(remote));
            //int rc = sendto(interface->sockfd, "update", 6, 0, (struct sockaddr *)&remote, sizeof(remote));
            if (rc == -1) {
                perror("sendto error");
            }
            //print_bytes(msg.message, msg.size);
        }

    }
    //printf("message is  %s , %d\n", message); 
    //print_bytes(msg.message, msg.size);

}


void triggered_update(struct Route_Table *node)
{

    for (int i = 0; i < self.input_number; i++)
        {
            send_update(&self.input[i], node);
        }
}



void garbage_collect(void* args)
{
    int *address = (int*) args;

    //check metric
    log_handler("ID:%d, Deleting route table entry\n", *address);
    //scan routetable
    struct Route_Table *item, *prior;

    pthread_mutex_lock(&write_route_table);   //lock


    item = routetable;
    prior = item;
    bool found = false;
    
    do
    {
        if(&item->address == address)
        {
            found = true;
            prior->next = item->next;
            free(item);
            break;
        }
        prior = item;
        item = item->next;
        
    }while (item != NULL);


    pthread_mutex_unlock(&write_route_table);   //unlock
}


void route_table_timeout(void* args)
{

    //int *address = (int*) args;

    struct Route_Table *node = (struct Route_Table*) args;

    //check metric
    log_handler("ID:%d, timeout, removing from route table\n", node->address);
    //scan routetable
    //struct Route_Table *item, *prior;

    pthread_mutex_lock(&write_route_table);        //lock

    /*
    item = routetable;
    prior = item;
    bool found = false;
    
    do
    {
        if(item == address)
        {
            found = true;
            item->metric = MAX_HOP + 1;
            item->garbage.tv_sec = GARBAGE;
            break;
        }
        prior = item;
        item = item->next;
        
    }while (item != NULL);
    */

    node->metric = MAX_HOP + 1;
    triggered_update(node);
    pthread_mutex_unlock(&write_route_table);     //unlock
    node->garbage.valid = true;
    set_time(&node->garbage, &node->garbage.timer_thread);
    log_handler("Setting timer %d for garbage collection %d\n", node->garbage.timer, node->address);
    
    if (pthread_join(node->garbage.timer_thread, NULL) != 0) 
            {
                perror("pthread_join");
                exit(1);
            }

    if (!node->garbage.valid)
    {
        log_handler("Need to cancel garbage process\n");
        node->timeout.timer.tv_sec = TIMEOUT;
        node->garbage.timer.tv_sec = GARBAGE;
        set_time(&node->timeout, &node->timeout.timer_thread);   //re-init the timer thread for timeout
    }

    
}

void add_route_table(struct ripEntry *re, int nexthop, int iface, int cost)
{
    //check metric

    //scan routetable
    struct Route_Table *item, *prior;

    pthread_mutex_lock(&write_route_table);   //lock

    item = routetable;
    prior = item;
    bool found = false;
    
    do
    {
        if(item->address == re->destination)
        {
            found = true;   
            //log_handler("found route %d at %d\n", item->address, item);             
            break;

        }
        prior = item;
        item = item->next;
        
    }while (item != NULL);
    
    if(found)
    {


        if(item->metric == re->metric + cost) 
        {

           
            item->timeout.timer.tv_sec = TIMEOUT;   //renew the timeout 
            item->flag = false;

            if (item->garbage.valid)
            {
                log_handler("Receiving valid updates, cancel garbaging\n");
                item->garbage.valid = false;
            }
            else  log_handler("ID:%d metric is the same, do nothing\n", re->destination);
            
        }
        else
        {
            if ((item->next_hop == nexthop) || (item->next_hop != nexthop && item->metric > re->metric + cost))
            {
                log_handler("Changing %d in routetable:\n", re->destination);
                if (re->metric + cost >= MAX_HOP + 1)
                {                    
                    item->metric = MAX_HOP + 1;
                    triggered_update(item);
                    item->timeout.timer.tv_sec = 0;
                    item->flag = true;
                }
                else 
                {
                    if (item->garbage.valid)
                    {
                        log_handler("Receiving valid updates, cancel garbaging\n");
                        item->garbage.valid = false;
                    }

                    item->metric = re->metric + cost;
                    item->next_hop = nexthop;
                    item->iface = iface;
                    item->flag = true;

                    item->timeout.timer.tv_sec = TIMEOUT;   //renew the timeout 
                }
            }

        }
    }

    else if (re->metric + cost <= MAX_HOP)
    {   
        log_handler("Adding %d to routetable:\n", re->destination);
        struct Route_Table *node = (struct Route_Table*)malloc(sizeof(struct Route_Table));
        node->address = re->destination;        
        node->next_hop = nexthop;
        node->iface = iface;
        node->flag = true;
        node->next = NULL;
        node->metric = re->metric + cost;   


        node->timeout.timer.tv_sec = TIMEOUT; 
        
 

        //node->timerdata.timer = &node->timeout;
        node->timeout.fun_ptr = &route_table_timeout;
        node->timeout.args = node; 
        node->timeout.valid = true;

        node->garbage.timer.tv_sec = GARBAGE;
        node->garbage.fun_ptr = &garbage_collect;
        node->garbage.args = &node->address; 
        node->garbage.valid = false;

        set_time(&node->timeout, &node->timeout.timer_thread);
        log_handler("Adding %d to timeout timer, route pointer is %d:\n", node->address, node);

        prior->next = node;
    }    


    pthread_mutex_unlock(&write_route_table);       //unlock
   

}




bool rip_head_validation(struct ripHeader *rh, int remoteid)
{
    //log_handler("rip head: %d, %d, %d\n", rh->command == COMMAND , rh->version == VERSION , rh->zero == 0);
    return (rh->command == COMMAND && rh->version == VERSION && rh->routerid > 0 && rh->routerid <= 64000 && rh->routerid == remoteid);
}
bool rip_entry_validation(struct ripEntry *re)
{
    return (re->addrfamily == ADDRFAMILY && re->zero == 0 && re->zero1 == 0 && re->zero2 == 0 && re->metric <= MAX_HOP + 1);
}

void decode_packet(char* packet, int size, struct Interface* interface)
{
    struct ripHeader rh;
    struct ripEntry re;
    int i = 4;
    if ((size - 4) % 20 || size < 4)
    {
        log_handler("incoming package length error, drop\n");
    }
    else
    {
        memcpy(&rh, (void*)packet, sizeof(rh));

        if (!rip_head_validation(&rh, interface->neighbor->routerid))
        {
            log_handler("head invalid, drop it\n");
        }
        else
        {
            while (size - i > 0)
            {
                memcpy(&re, (void*)packet + i, sizeof(re));
                log_handler("GET ADDRESS: %d, metric: %d, next_hop: %d\n", re.destination, re.metric, rh.routerid);
                if (!rip_entry_validation(&re))
                {
                    log_handler("metric out of range, drop\n");
                }
                else 
                {
                    add_route_table(&re, rh.routerid, interface->port, interface->neighbor->metric);
                }
                i += sizeof(re);
            }
        }
    }   
}












void init()
{
    self.routerid = -1;
    self.input_number = 0;
    self.output_number = 0;
    self.routerid_status = false;
    self.input = NULL;
    self.output = NULL;

    routetable = (struct Route_Table*)malloc(sizeof(struct Route_Table));
    routetable->next = NULL;
    routetable->address = self.routerid;
    routetable->metric = 0;
    routetable->next_hop = 0;
    routetable->iface = 0;
    routetable->flag = false;

    pthread_mutex_t write_route_table = PTHREAD_MUTEX_INITIALIZER;

}


bool check_port(int port)
{
    return (port >= 1024 && port <= 64000);
}

bool check_routerid(int routerid)
{
    return (routerid > 0 && routerid <= 64000);
}

bool check_metric(int metric)
{
    return (metric >= 0 && metric <= 16);
}



int readConfig(char *cfg_file, struct ConfigItem *item)
{

    char *mark;
    char *line = NULL;   //every line of config file
    size_t len = 0;
    char *name = NULL;
    char *content = NULL;
    char *content_collect = NULL;
    int n = 1;

    FILE *fp = fopen(cfg_file, "r");
    
    if (fp == NULL) {
        exit(EXIT_FAILURE);
    }
        
    while ((len = getline(&line, &len, fp)) != -1) 
    {
        if(*line != '#')  //bypass comment
        {
            if (line[len - 1] == '\n') 
            {
                line[len - 1] = '\0';
            }

            mark = strchr(line, ':');
            if (mark != NULL)
            {
                //init
                name = (char*)malloc(sizeof(char) * (mark - line + 1));
                content = (char*)malloc(sizeof(char) * (len - (mark - line) + 1));
                content_collect = content;
                memset(name, '\0', mark - line + 1);
                memset(content, '\0', (len - (mark - line) + 1)); 

                strncpy(name, line, mark - line);
                strcpy(content, mark + 1);


                if (strcmp(name,configName[0]) == 0)               //router-id field
                {
                    if (item->routerid_status)
                    {
                        log_handler("WARNING: Duplicate router-id found at line %d!\n", n);
                    }  
                    else if (!check_routerid(atoi(content)))
                    {
                        log_handler("ERROR: router-id invalid at line %d\n", n);
                        exit_program();
                    }
                    else
                    {
                        item->routerid = atoi(content); 
                        item->routerid_status = true;
                        log_handler("router-id is : %d\n", item->routerid);                        
                    }
                    
                }
                else if (strcmp(name, configName[1]) == 0)         //input field
                {
                    char *ptr;
                    while ((ptr = strtok(content, ",")) != NULL)
                    {
                        if(!check_port(atoi(ptr)))
                        {
                            log_handler("ERROR: port invalid at line %d\n", n);
                            exit_program();
                        }
                        else
                        {
                            item->input_number++;
                            item->input = (struct Interface*)realloc(item->input, sizeof(struct Interface) * item->input_number);
                            if (item->input == NULL)
                            {
                                log_handler("realloc error/n/n");
                            }
                            item->input[item->input_number - 1].port = atoi(ptr); 
                            item->input[item->input_number - 1].neighbor = NULL;
                            item->input[item->input_number - 1].found_peer = false;
                            item->input[item->input_number - 1].sockfd = 0;
                            content = NULL;

                            log_handler("INPUT:%d\n", item->input[item->input_number - 1].port);
                        }

                    }

                }
                else if (strcmp(name, configName[2]) == 0)         //output field
                {
                    char *ptr;
                    while ((ptr = strtok(content, ",")) != NULL)
                    {
                        char *dash;
                        char temp[8];
                        
                        item->output_number++;
                        item->output = (struct Peer*)realloc(item->output, sizeof(struct Peer) * item->output_number);
                        
                        memset(temp, '\0', sizeof(temp));            //read peer port
                        dash = strchr(ptr, '-');
                        strncpy(temp, ptr, dash - ptr);
                        if (!check_port(atoi(temp)))
                        {
                            log_handler("ERROR: port invalid at line %d\n", n);
                            exit_program();
                        }
                        
                        item->output[item->output_number - 1].port = atoi(temp);

                        memset(temp, '\0', sizeof(temp));            //read peer metric
                        ptr = dash + 1;                             
                        dash = strchr(ptr, '-');
                        strncpy(temp, ptr, dash - ptr);
                        if (!check_metric(atoi(temp)))
                        {
                            log_handler("ERROR: metric invalid at line %d\n", n);
                            exit_program();
                        }
                        item->output[item->output_number - 1].metric = atoi(temp);

                        memset(temp, '\0', sizeof(temp));           //read peer router-id 
                        strcpy(temp, dash + 1);
                        if (!check_routerid(atoi(temp)))
                        {
                            log_handler("ERROR: router-id invalid at line %d\n", n);
                            exit_program();
                        }
                        item->output[item->output_number - 1].routerid = atoi(temp);


                        content = NULL;
                        log_handler("OUTPUT: port: %d, metric: %d, peer: %d\n",  
                            item->output[item->output_number - 1].port, 
                            item->output[item->output_number - 1].metric, 
                            item->output[item->output_number - 1].routerid);
                    }

                }

                free(name);
                free(content_collect);
            }
            
        }
        n++;   //line number
 
    }

    free(line);
    fclose(fp);
}


bool check_receive_match(int port)
{
    bool result = false;
    for (int i = 0; i < self.output_number; i++)
    {
        if (port == self.output[i].port)
        {
            result = true;
            break;
        }
    }
    return result;
}


void listen_port(struct Interface *interface)
{
    struct timeval recv_timeout;   //set timeout value
    interface->sockfd = socket(AF_INET,SOCK_DGRAM,0);   //create UDP socket with default protocol    
    
    fd_set recvfd, sendfd;

    struct sockaddr_in local, remote; 
    local.sin_family = AF_INET;          /* communicate using internet address */
    local.sin_addr.s_addr = INADDR_ANY; /* accept all calls                   */
    local.sin_port = htons(interface->port); /* this is port number                */

    socklen_t remote_len;

    char buf[BUF_SIZE];

    int i, rc, remote_port;

    rc = bind(interface->sockfd,(struct sockaddr *)&local,sizeof(local)); /* bind address to socket   */ 
    if(rc == -1) { // Check for errors
        perror("bind");
        exit(1);
    }

    log_handler("Listening on port %d\n", interface->port);




    while(1) 
    {
        FD_ZERO(&recvfd);        
        FD_SET(interface->sockfd, &recvfd); 

        FD_ZERO(&sendfd);
        FD_SET(interface->sockfd, &sendfd); 

        recv_timeout.tv_sec = TIMEOUT;
        recv_timeout.tv_usec = 0;
        switch(select(interface->sockfd + 1, &recvfd, NULL, NULL, &recv_timeout))
        {
            case -1:
                log_handler("select error\n");
                break;
            case 0:
                log_handler("TIMEOUT: Listening on port %d\n", interface->port);
                break;
            default:
                if (FD_ISSET(interface->sockfd, &recvfd))
                {
                    rc = recvfrom(interface->sockfd, buf, BUF_SIZE, 0, (struct sockaddr *)&remote, &remote_len);
                    if (rc == -1) {
                        log_handler("recvfrom error");
                    }      
                    remote_port = ntohs(remote.sin_port); 
                    if (check_receive_match(remote_port)) 
                    {
                        //printf("Received %s at PORT %d\n",buf, remote_port);
                        struct Peer *peer; 
                        for(i = 0; i < self.output_number; i++)
                        {
                            if (self.output[i].port == remote_port)
                            {
                                peer = &self.output[i];
                                break;
                            }
                        }
                        interface->neighbor = peer;
                        interface->found_peer = true;

                        decode_packet(buf, rc, interface);
                        
                    }
                }
                

        }

    }


    close(interface->sockfd);

}


void print_bytes(unsigned char *bytes, size_t num_bytes) {
    
  for (size_t i = 0; i < num_bytes; i++) {
    printf("%*u ", 3, bytes[i]);
  }
  printf("\n");
}


void print_route_table()
{
    printf("\n=======Route Table=======\n");
    printf("%-10s%-10s%-10s%-10s%-10s%-10s%-10s%-10s\n","Dest", "Metric", "NextHop", "ChgFlag", "Iface", "Timeout", "Garbage", "Pointer");
    struct Route_Table *item = routetable->next;
    while(item != NULL)
    {
        char flag = 'N';
        if (item->flag) flag = 'Y';
        printf("%-10d%-10d%-10d%-10c%-10d%-10d%-10d%-10d\n",item->address, item->metric, item->next_hop, flag, item->iface, item->timeout.timer.tv_sec, item->garbage.timer.tv_sec, item);

        item = item->next;
    }
    printf("=======Route Table END=======\n");
}

void* listen_process(void *argv)
{
    
    struct Interface *interface = (struct Interface *)argv;
    
    

    listen_port(interface);
       

    pthread_exit(0);
}

void* update_process()
{
    while(1)
    {
        for (int i = 0; i < self.input_number; i++)
        {
            send_update(&self.input[i], NULL);
        }
        print_route_table();
        srand(time(NULL) + self.routerid);
        int r = rand() % (UPDATE * 1000000 / 6);
        log_handler("random sleep %d\n", r);
        usleep(r);               //random update
        sleep (UPDATE);          
    }
    pthread_exit(0);
}










int main(int argc, char **argv) 
{

    pthread_t listener[self.input_number];   //create PIDs
    pthread_t updater;

    init();

    if (argc != 2) 
    {
        fprintf(stderr, "usage: ./rip configure_file\n");
        exit(1);
    }

    readConfig(argv[1], &self);
    
    routetable->address = self.routerid;


    
    for(int i=0; i<self.input_number; i++)
    {
        if (pthread_create(&listener[i],NULL,listen_process,&self.input[i]) != 0)
        {
            perror("create");
            exit(1);
        }
    }


    log_handler("Sending: \n===============================\n");
    for(int i=0; i<self.output_number; i++)
    {
         log_handler("OUTPUT: port: %d, metric: %d, peer: %d\n",  self.output[i].port, self.output[i].metric, self.output[i].routerid);
    }
    log_handler("================================\n");

    
    if (pthread_create(&updater,NULL,update_process, NULL) != 0)
        {
            perror("create");
            exit(1);
        }


    for(int i=0; i<self.input_number; i++)
    {
        if (pthread_join(listener[i], NULL) != 0) {
            perror("pthread_join");
            exit(1);
        }
    }

    
    exit_program();       //free resources, exit threads, then exit the program
    return 0;    
}


