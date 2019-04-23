/*
Use gcc rip.c -o rip -lpthread to compile



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

#include <fcntl.h>

#define BUF_SIZE 1024
#define MAX_HOP 15
#define INTERVAL 30
#define TIMEOUT 30

char *configName[] =  {"router-id", "input-ports", "outputs"};

//============== Global Variables ============================

struct Peer
{
    int port;
    int metric;
    int peer_id;
};

struct Interface
{
    int port;
    int sockfd;
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

struct Route_Table
{
    int address;
    uint32_t metric;
    int next_hop;
    bool flag;

};

struct Route_Table *router;

struct Timer_Struct
{
    struct timeval timer;
    void (*fun_ptr)();
    void *args;
    bool *cancel;      //necessary?
};

pthread_t *threads;

//=============================================================

void* time_handler(void *args)
{
    struct Timer_Struct *timerdata = (struct Timer_Struct *)args;  //formating the passing args
    printf("Starting Timer, %d\n", timerdata->timer.tv_sec);       //debug
    time_t nowtime, starttime;        //set time stamp
    time(&starttime);      //remember start time
    while (1)
    {
        sleep(1);         //wait 1 second
        time(&nowtime);   //check current time
        if (nowtime - starttime >= timerdata->timer.tv_sec) //if time elapse >= timer value
        {
            (timerdata->fun_ptr)(timerdata->args);       //call the function
            break;                        //quit loop
        }
    }

    pthread_exit(0);                      //exit thread
}


void set_time(struct timeval *timer, void (*fun_ptr)(), void *args)
{
    pthread_t timer_thread;          //create timer thread id
    struct Timer_Struct timerdata;    //initial passing args
    timerdata.timer = *timer;
    timerdata.fun_ptr = fun_ptr;
    timerdata.args = args;


    if (pthread_create(&timer_thread,NULL,time_handler,&timerdata) != 0) //create timer thread
    {
            perror("TimeHandler");
            exit(1);
    }

    if (pthread_join(timer_thread, NULL) != 0) {       //waiting for timer thread quit
            perror("pthread_join");
            exit(1);
        }
    printf("Timer quit success\n");

}




void init(struct ConfigItem *item)
{
    item->routerid = -1;
    item->input_number = 0;
    item->output_number = 0;
    item->routerid_status = false;
    item->input = NULL;
    item->output = NULL;
}


int readConfig(char *cfg_file, struct ConfigItem *item)
{

    char *mark;
    char *line = NULL;   //every line of config file
    size_t len = 0;
    char *name = NULL;
    char *content = NULL;

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
                memset(name, '\0', mark - line + 1);
                memset(content, '\0', (len - (mark - line) + 1)); 

                strncpy(name, line, mark - line);
                strcpy(content, mark + 1);


                if (strcmp(name,configName[0]) == 0)
                {
                    if (item->routerid_status)
                    {
                        printf("WARNING: Duplicate router-id found!! check config file\n");
                    }
                    else
                    {

                        item->routerid = atoi(content);
                        item->routerid_status = true;
                        printf("router-id is : %d\n", item->routerid);                        
                    }
                    
                }
                else if (strcmp(name, configName[1]) == 0)
                {
                    char *ptr;
                    while ((ptr = strtok(content, ",")) != NULL)
                    {
                        item->input_number++;
                        item->input = (struct Interface*)realloc(item->input, sizeof(struct Interface) * item->input_number);
                        if (item->input == NULL)
                        {
                            printf("realloc error/n/n");
                        }
                        item->input[item->input_number - 1].port = atoi(ptr);                       
                        content = NULL;

                        printf("INPUT:%d\n", item->input[item->input_number - 1].port);
                    }

                }
                else if (strcmp(name, configName[2]) == 0)
                {
                    char *ptr;
                    while ((ptr = strtok(content, ",")) != NULL)
                    {
                        char *dash;
                        char temp[8];
                        memset(temp, '\0', sizeof(temp));

                        item->output_number++;
                        item->output = (struct Peer*)realloc(item->output, sizeof(struct Peer) * item->output_number);
                        

                        dash = strchr(ptr, '-');
                        strncpy(temp, ptr, dash - ptr);
                        item->output[item->output_number - 1].port = atoi(temp);

                        memset(temp, '\0', sizeof(temp));

                        ptr = dash + 1;
                        dash = strchr(ptr, '-');
                        strncpy(temp, ptr, dash - ptr);
                        item->output[item->output_number - 1].metric = atoi(temp);

                        memset(temp, '\0', sizeof(temp));
                        strcpy(temp, dash + 1);
                        item->output[item->output_number - 1].peer_id = atoi(temp);

                        content = NULL;
                        printf("OUTPUT: port: %d, metric: %d, peer: %d\n",  
                            item->output[item->output_number - 1].port, 
                            item->output[item->output_number - 1].metric, 
                            item->output[item->output_number - 1].peer_id);
                    }

                }

                free(name);
                free(content);
            }
            
        }
 
    }

    //cleanup

    free(line);
    fclose(fp);
}


bool check_valid_receive(int port)
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

    char buf[80];

    int i, rc, remote_port;

    rc = bind(interface->sockfd,(struct sockaddr *)&local,sizeof(local)); /* bind address to socket   */ 
    if(rc == -1) { // Check for errors
        perror("bind");
        exit(1);
    }

    printf("Listening on port %d\n", interface->port);




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
                printf("select error\n");
                break;
            case 0:
                printf("TIMEOUT: Listening on port %d\n", interface->port);
                break;
            default:
                if (FD_ISSET(interface->sockfd, &recvfd))
                {
                    rc = recvfrom(interface->sockfd, buf, 80, 0, (struct sockaddr *)&remote, &remote_len);
                    if (rc == -1) {
                        perror("recvfrom error");
                    }      
                    remote_port = ntohs(remote.sin_port); 
                    if (check_valid_receive(remote_port)) printf("Received %s at PORT %d\n",buf, remote_port);
                }



                

        }
        /*sa_len = sizeof(sa);
        n = recvfrom(sockfd, buf, 80, 0, (struct sockaddr *)&sa, &sa_len);
        if (n == -1) {
            perror("recvfrom error");
        }
        printf("received at PORT %d\n",ntohs(sa.sin_port));
        for (i = 0; i < n; i++) {
            buf[i] = toupper(buf[i]);
        }
        n = sendto(sockfd, buf, n, 0, (struct sockaddr *)&sa, sizeof(sa));
        if (n == -1) {
            perror("sendto error");
        }*/


    }


    close(interface->sockfd);

}


void update_ports(struct Interface *interface)
{
    struct sockaddr_in remote; 


    remote.sin_family = AF_INET;          /* communicate using internet address */
    remote.sin_addr.s_addr = INADDR_ANY; /* accept all calls                   */
    remote.sin_port = htons(self.output[0].port); /* this is port number                */


    int rc = sendto(interface->sockfd, "update", 6, 0, (struct sockaddr *)&remote, sizeof(remote));
    if (rc == -1) {
    perror("sendto error");
    }
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
            update_ports(&self.input[i]);
        }
        sleep (5);
    }
    pthread_exit(0);
}

void fun(void* args) 
{ 
    int *a = (int*) args;
    printf("function called success %d \n", *a); 
} 


int main(int argc, char **argv) 
{

    pthread_t listener[self.input_number];   //create PIDs
    pthread_t updater;

    init(&self);

    if (argc != 2) 
    {
        fprintf(stderr, "usage: ./rip configure_file\n");
        exit(1);
    }

    readConfig(argv[1], &self);
    
    struct timeval temp;
    temp.tv_sec = 12;
    temp.tv_usec = 0;
    
    int a = 12;
    set_time(&temp, &fun, &a);


    for(int i=0; i<self.input_number; i++)
    {
        if (pthread_create(&listener[i],NULL,listen_process,&self.input[i]) != 0)
        {
            perror("create");
            exit(1);
        }
    }


    printf("Sending: \n===============================\n");
    for(int i=0; i<self.output_number; i++)
    {
         printf("OUTPUT: port: %d, metric: %d, peer: %d\n",  self.output[i].port, self.output[i].metric, self.output[i].peer_id);
    }
    printf("================================\n");

    
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



    free(self.input);
    free(self.output);
    return 0;
}


