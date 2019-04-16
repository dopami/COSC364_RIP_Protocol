#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h> 
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>

#define BUF_SIZE 1024
#define MAX_HOP 15

char *configName[] =  {"router-id", "input-ports", "outputs"};

//============== Global Variables ============================

struct Contact
{
    int port;
    int metric;
    int peer_id;
};

struct ConfigItem
{
    int routerid;
    int input_number;
    int output_number;
    bool routerid_status;
    int *input;
    struct Contact *output;
}self;

pthread_t *threads;

//=============================================================

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
                        item->input = (int*)realloc(item->input, sizeof(int) * item->input_number);
                        if (item->input == NULL)
                        {
                            printf("realloc error/n/n");
                        }
                        item->input[item->input_number - 1] = atoi(ptr);                       
                        content = NULL;

                        printf("INPUT:%d\n", item->input[item->input_number - 1]);
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
                        item->output = (struct Contact*)realloc(item->output, sizeof(struct Contact) * item->output_number);
                        

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





void listen_port(int port)
{
  
    int sockfd = socket(AF_INET,SOCK_DGRAM,0);   //create UDP socket with default protocol    
    
    struct sockaddr_in sa, ca; 

    char buf[80];
    char str[INET_ADDRSTRLEN];
    int i, n;

    socklen_t ca_len;

    sa.sin_family = AF_INET;          /* communicate using internet address */
    sa.sin_addr.s_addr = INADDR_ANY; /* accept all calls                   */
    sa.sin_port = htons(port); /* this is port number                */
        
    int rc = bind(sockfd,(struct sockaddr *)&sa,sizeof(sa)); /* bind address to socket   */ 
    if(rc == -1) { // Check for errors
        perror("bind");
        exit(1);
    }
        
    while(1) 
    {
        ca_len = sizeof(ca);
        n = recvfrom(sockfd, buf, 80, 0, (struct sockaddr *)&ca, &ca_len);
        if (n == -1) {
            perror("recvfrom error");
        }
        printf("received at PORT %d\n",ntohs(ca.sin_port));
        for (i = 0; i < n; i++) {
            buf[i] = toupper(buf[i]);
        }
        n = sendto(sockfd, buf, n, 0, (struct sockaddr *)&ca, sizeof(ca));
        if (n == -1) {
            perror("sendto error");
        }
    }


    close(sockfd);

}



void* listen_process(void *argv)
{
    
    int *port = (int *)argv;
    
    printf("Listening on port %d\n", *port);
    while(1) 
    {   // forever
        listen_port(*port);
       
    }

    pthread_exit(0);
}



int main(int argc, char **argv) 
{

    pthread_t listener[self.input_number];   //create PIDs

    init(&self);

    if (argc != 2) 
    {
        fprintf(stderr, "usage: ./rip configure_file\n");
        exit(1);
    }

    readConfig(argv[1], &self);
    
    printf("Listening: \n===============================\n");
    for(int i=0; i<self.input_number; i++)
    {
        

        if (pthread_create(&listener[i],NULL,listen_process,&self.input[i]) != 0)
        {
            perror("create");
            exit(1);
        }

    }
    printf("================================\n");

    printf("Sending: \n===============================\n");
    for(int i=0; i<self.output_number; i++)
    {
         printf("OUTPUT: port: %d, metric: %d, peer: %d\n",  self.output[i].port, self.output[i].metric, self.output[i].peer_id);
    }
    printf("================================\n");

    


    
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


