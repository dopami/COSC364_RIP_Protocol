/*
Use gcc rip.c -o rip -lpthread to compile

Author: BR

Reference: RFC 2453 RIP Version 2

This code is the assignment of COSC364 in University of Canterbury


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

bool showlog = false;
bool gracequit = false;
pthread_mutex_t screen;   //display will not be interrupted
pthread_mutex_t access_route_table;  //protect route_table so only one thread can access at a time


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
    pthread_mutex_t send_socket;   //socket send is not an atomic operation
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
    pthread_mutex_t change_time;
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
                     
}*routetable;




struct RIP_Header {
  byte command; 		    // 1-REQUEST, 2-RESPONSE 
  byte version;             // 2 in this assignment
  short int routerid;       // it should be zero, but we used as router-id
};


struct RIP_Entry {
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

void exit_program()                        //release all resources, wait for thread end, free memory
{
    free(self.input);
    free(self.output);

    exit(0);
}

void log_handler(const char *fmt, ...)     //just like printf, but add time tag and switch on/off
{
    if (showlog)
    {
        time_t t;
        time(&t);
        struct tm *tmp_time = localtime(&t);
        char s[100];

        pthread_mutex_lock(&screen);       //if no one is outputing to screen, start print log

        strftime(s, sizeof(s), "%04Y%02m%02d-%H:%M:%S", tmp_time);
        printf("[LOG-%s]: ", s);          //log head with time tag
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);                 //print log body with format variables
        va_end(ap);

        pthread_mutex_unlock(&screen);     //release lock
    }
}

void* time_handler(void *args)
{
    struct Timer_Struct *timerdata = (struct Timer_Struct *)args;  //formating the passing args
    log_handler("Starting Timer, %d\n", timerdata->timer.tv_sec); 

    
    while (timerdata->timer.tv_sec > 0 && timerdata->valid)  //if we don't need to cancel the timer, or time still left
    {
        log_handler("time left %d\n", timerdata->timer.tv_sec);
        pthread_mutex_lock(&timerdata->change_time);    
        timerdata->timer.tv_sec --;   //adjust timer
        pthread_mutex_unlock(&timerdata->change_time); 
        sleep(1);         //wait 1 second 
    }

    if (timerdata->valid)  (timerdata->fun_ptr)(timerdata->args);       //if we don't need to cancel the timer, call the function
    
    pthread_exit(0);                      //exit thread

    
}


void set_time(struct Timer_Struct *timerdata, pthread_t *timer_thread)
{


    log_handler("Set Timer %d\n", timerdata->timer.tv_sec); 
    if (pthread_create(timer_thread,NULL,time_handler,timerdata) != 0) //create timer thread
    {
            perror("TimeHandler");
            exit(1);
    }

    log_handler("Timer quit success\n");

}


void packet_header(struct packet *msg, struct RIP_Header rh)
{   

    msg->size += sizeof(rh);
    msg->message = (char*)malloc(msg->size);
    memset(msg->message, '\0', msg->size);
    memcpy(msg->message, (void*)&rh, sizeof(rh));
}

void packet_entry(struct packet *msg, struct RIP_Entry re)
{   

    
    msg->message = (char*)realloc(msg->message, msg->size + sizeof(re));
    memset(msg->message + msg->size, '\0', sizeof(re));
    memcpy(msg->message + msg->size, (void*)&re, sizeof(re));
    msg->size += sizeof(re);
}



void generate_update(struct packet *msg, int nexthop, struct Route_Table *node) 
{ 
    struct RIP_Header rh;
    struct RIP_Entry re;
    rh.command = COMMAND;
    rh.version = VERSION;
    rh.routerid = self.routerid;
    packet_header(msg, rh);

    re.addrfamily = ADDRFAMILY;
    re.zero = 0;
    re.zero1 = 0;
    re.zero2 = 0;

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
        pthread_mutex_lock(&interface->send_socket);
        rc = sendto(interface->sockfd, msg.message, msg.size, 0, (struct sockaddr *)&remote, sizeof(remote));
        pthread_mutex_unlock(&interface->send_socket);  
        log_handler("Sending msg to %d from port %d\n", interface->neighbor->routerid, interface->port);      
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
            pthread_mutex_lock(&interface->send_socket);
            rc = sendto(interface->sockfd, msg.message, msg.size, 0, (struct sockaddr *)&remote, sizeof(remote));
            pthread_mutex_unlock(&interface->send_socket);
            
            if (rc == -1) {
                perror("sendto error");
            }
        }
        log_handler("Sending all route entries from port %d\n", interface->port);

    }
    


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
    struct Route_Table *node = (struct Route_Table*) args;

    //check metric
    log_handler("ID:%d, Deleting route table entry\n", node->address);
    //scan routetable
    struct Route_Table *item, *prior;

    pthread_mutex_lock(&access_route_table);   //lock
    item = routetable;
    prior = item;    
    do
    {
        if(item == node)
        {

            prior->next = node->next;
            free(node);
            break;
        }
        prior = item;
        item = item->next;
    }while (item != NULL);
    pthread_mutex_unlock(&access_route_table);   //unlock

    log_handler("Route table entry deleted\n");
}


void route_table_timeout(void* args)
{


    struct Route_Table *node = (struct Route_Table*) args;


    log_handler("ID:%d, timeout, removing from route table\n", node->address);
 

    pthread_mutex_lock(&access_route_table);        //lock
    node->metric = MAX_HOP + 1;
    triggered_update(node);
    pthread_mutex_unlock(&access_route_table);     //unlock

    pthread_mutex_lock(&node->garbage.change_time); 
    node->garbage.valid = true;
    pthread_mutex_unlock(&node->garbage.change_time); 


  
    set_time(&node->garbage, &node->garbage.timer_thread);

    log_handler("Setting timer %d for garbage collection %d\n", node->garbage.timer, node->address);
    
    if (pthread_join(node->garbage.timer_thread, NULL) != 0) 
            {
                perror("pthread_join");
                exit(1);
            }

    if (!node->garbage.valid)
    {
        log_handler("Canceling garbage process\n");
        pthread_mutex_lock(&node->timeout.change_time);  
        node->timeout.timer.tv_sec = TIMEOUT;
        pthread_mutex_unlock(&node->timeout.change_time);

        pthread_mutex_lock(&node->garbage.change_time);
        node->garbage.timer.tv_sec = GARBAGE;
        pthread_mutex_unlock(&node->garbage.change_time);

        log_handler("Timeout and Garbage Reset\n");
        set_time(&node->timeout, &node->timeout.timer_thread);   //re-init the timer thread for timeout
    }

    
}

void add_route_table(struct RIP_Entry *re, int nexthop, int iface, int cost)
{

    struct Route_Table *item, *prior;


    item = routetable;
    prior = item;
    bool found = false;
    

    pthread_mutex_lock(&access_route_table);   //lock

    do
    {
        if(item->address == re->destination)
        {
            found = true;                
            break;

        }
        prior = item;
        item = item->next;
        
    }while (item != NULL);
    
    if(found)
    {
        

        if(item->metric == re->metric + cost) 
        {

            pthread_mutex_lock(&item->timeout.change_time); 
            item->timeout.timer.tv_sec = TIMEOUT;   //renew the timeout 
            pthread_mutex_unlock(&item->timeout.change_time); 

            item->flag = false;

            if (item->garbage.valid)
            {
                log_handler("Receiving valid updates, cancel garbaging\n");
                pthread_mutex_lock(&item->garbage.change_time); 
                item->garbage.valid = false;
                pthread_mutex_unlock(&item->garbage.change_time); 
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

                    pthread_mutex_lock(&item->timeout.change_time); 
                    item->timeout.timer.tv_sec = 0;
                    pthread_mutex_unlock(&item->timeout.change_time); 

                    item->flag = true;
                }
                else 
                {
                    if (item->garbage.valid)
                    {
                        log_handler("Receiving valid updates, cancel garbaging\n");
                        pthread_mutex_lock(&item->garbage.change_time); 
                        item->garbage.valid = false;
                        pthread_mutex_unlock(&item->garbage.change_time); 
                    }

                    item->metric = re->metric + cost;
                    item->next_hop = nexthop;
                    item->iface = iface;
                    item->flag = true;

                    pthread_mutex_lock(&item->timeout.change_time); 
                    item->timeout.timer.tv_sec = TIMEOUT;   //renew the timeout
                    pthread_mutex_unlock(&item->timeout.change_time);  
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
        
        node->metric = re->metric + cost;   

        node->timeout.timer.tv_sec = TIMEOUT; 
        
 

        //node->timerdata.timer = &node->timeout;
        node->timeout.fun_ptr = &route_table_timeout;
        node->timeout.args = node; 
        node->timeout.valid = true;
        pthread_mutex_init(&node->timeout.change_time, NULL);

        node->garbage.timer.tv_sec = GARBAGE;
        node->garbage.fun_ptr = &garbage_collect;
        node->garbage.args = node; 
        node->garbage.valid = false;
        pthread_mutex_init(&node->garbage.change_time, NULL);


        set_time(&node->timeout, &node->timeout.timer_thread);
        log_handler("Adding %d to timeout timer, route pointer is %d:\n", node->address, node);

        
        
        item = routetable->next;
        prior = routetable;

        while (item != NULL)            //find the place to insert the router table node
        {
            if (item->address >= node->address) break;

            prior = item;
            item = item->next;
        }

        node->next = item;
        prior->next = node;
    }    


    pthread_mutex_unlock(&access_route_table);       //unlock
   

}




bool rip_head_validation(struct RIP_Header *rh, int remoteid)
{
    //log_handler("rip head: %d, %d, %d\n", rh->command == COMMAND , rh->version == VERSION , rh->zero == 0);
    return (rh->command == COMMAND && rh->version == VERSION && rh->routerid > 0 && rh->routerid <= 64000 && rh->routerid == remoteid);
}
bool rip_entry_validation(struct RIP_Entry *re)
{
    return (re->addrfamily == ADDRFAMILY && re->zero == 0 && re->zero1 == 0 && re->zero2 == 0 && re->metric <= MAX_HOP + 1);
}

void decode_packet(char* packet, int size, struct Interface* interface)
{
    struct RIP_Header rh;
    struct RIP_Entry re;
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

    pthread_mutex_t access_route_table = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t screen = PTHREAD_MUTEX_INITIALIZER;
}


bool check_port(int port)
{
    bool output = true;
    if (port < 1024 || port > 64000)
    {
        output = false;
    }
    else
    {
        for (int i=0; i<self.input_number; i++)
        {
            if (port == self.input[i].port)
            {
                output = false;
                log_handler("Port %d is already occured in input field\n", port);
                break;
            }
        }

        if (output)
        {
            for (int i=0; i<self.output_number; i++)
            {
                if (port == self.output[i].port)
                {
                    output = false;
                    log_handler("Port %d is already occured in output field\n", port);
                    break;
                }
            }
        }
    }
    
    return output;
}

bool check_routerid(int routerid)
{
    bool output = true;
    if (routerid <= 0 || routerid > 64000)
    {
        output = false;
    }
    else
    {

        if (routerid == self.routerid)
        {
            output = false;
            log_handler("ID %d is confict to local router-id\n", routerid);
            
        }
        

        if (output)
        {
            for (int i=0; i<self.output_number; i++)
            {
                if (routerid == self.output[i].routerid)
                {
                    output = false;
                    log_handler("ID %d is already occured in output field\n", routerid);
                    break;
                }
            }
        }
    }

    return output;
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
                            pthread_mutex_init(&item->input[item->input_number - 1].send_socket, NULL);
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
                        if (dash == NULL)
                        {
                            log_handler("ERROR: format error at line %d\n", n);
                            exit_program();
                        }
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
                        if (dash == NULL)
                        {
                            log_handler("ERROR: format error at line %d\n", n);
                            exit_program();
                        }
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

    if (self.routerid == -1) 
    {
        log_handler("ERROR: Can't find router-id\n");
        exit_program();
    }
    if (self.input_number == 0) 
    {
        log_handler("ERROR: Can't find input field\n");
        exit_program();
    }
    if (self.output_number == 0) 
    {
        log_handler("ERROR: Can't find output field\n");
        exit_program();
    }

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




    while(!gracequit) 
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
    
    printf("\n===================  Route Table  =====================\n");
    printf("%-6s%-8s%-9s%-9s%-7s%-9s%-10s\n","Dest", "Metric", "NextHop", "ChgFlag", "Iface", "Timeout", "Garbage");
    printf("-------------------------------------------------------\n");
    struct Route_Table *item = routetable->next;
    pthread_mutex_lock(&access_route_table); 
    while(item != NULL)
    {
        char flag = 'N';
        if (item->flag) flag = 'Y';
        
        printf("%-6d%-8d%-9d%-9c%-7d%-9ld%-10ld\n",item->address, item->metric, item->next_hop, flag, item->iface, item->timeout.timer.tv_sec, item->garbage.timer.tv_sec);
        
        item = item->next;
    }
    pthread_mutex_unlock(&access_route_table); 
    printf("=================  Route Table END  ===================\n");
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
            pthread_mutex_lock(&access_route_table); 
            send_update(&self.input[i], NULL);
            pthread_mutex_unlock(&access_route_table); 
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


void* CLI_daemon()    //very silly approach for cisco-like CLI(command line interface)
{
    printf("Router_%d>",self.routerid);  //prompt with hostname
    while(!gracequit)
    {
        char cli_command[128];
        fgets(cli_command, 128, stdin);     //read command
        pthread_mutex_lock(&screen);


        if (strcmp(cli_command, "terminal monitor\n") == 0)    //ter mo
        {
            showlog = true;
            printf("Logging displays Enable\n");
        }
        else if (strcmp(cli_command, "terminal no monitor\n") == 0)   //ter no mo
        {
            showlog = false;
            printf("Logging displays Disable\n");
        }
        else if (strcmp(cli_command, "exit\n") == 0)   //exit
        {
            gracequit = true;
            printf("Shutting Down\n");
        }
        else if (strcmp(cli_command, "show run\n") == 0)    //show run
        {
            printf("Router id: %d\n", self.routerid);
            printf("Input UDP Number: ");
            for (int i = 0; i < self.input_number; i++)
            {
                printf("%d ",self.input[i].port);
            }
            printf("\n");
            printf("Neighbors:\n");
            for(int i=0; i<self.output_number; i++)
            {
                printf("    Router-id: %d, port: %d, metric: %d\n", self.output[i].routerid, self.output[i].port, self.output[i].metric);
            }

        }
        else if (strcmp(cli_command, "help\n") == 0 || strcmp(cli_command, "?\n") == 0)       //help
        {
            printf("%-30s%s\n", "terminal monitor", "Shows debug logging on screen");
            printf("%-30s%s\n", "terminal no monitor", "Hide debug logging on screen");
            printf("%-30s%s\n", "show run", "Shows RIP running config");
            printf("%-30s%s\n", "exit", "Release all resources and quit the program");
            

        }

        else if (strcmp(cli_command, "\n") != 0)
        {
            printf("Unknown command %sUse help command\n", cli_command);
        }


        printf("Router_%d>",self.routerid);
        pthread_mutex_unlock(&screen);
    }
}





int main(int argc, char **argv) 
{

    pthread_t listener[self.input_number];   //create PIDs
    pthread_t updater;
    pthread_t cli;

    init();

    if (argc != 2) 
    {
        fprintf(stderr, "usage: ./rip configure_file\n");
        exit(1);
    }

    showlog = true;
    readConfig(argv[1], &self);
    showlog = false;

    routetable->address = self.routerid;


    if (pthread_create(&cli,NULL,CLI_daemon, NULL) != 0)
        {
            perror("create");
            exit(1);
        }


    
    for(int i=0; i<self.input_number; i++)
    {
        if (pthread_create(&listener[i],NULL,listen_process,&self.input[i]) != 0)
        {
            perror("create");
            exit(1);
        }
    }



    
    
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

    if (pthread_join(cli, NULL) != 0) {
            perror("pthread_join");
            exit(1);
    }



    
    exit_program();       //free resources, exit threads, then exit the program
    return 0;    
}


