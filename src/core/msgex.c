#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main()
{
    //管道1,用于子进程发送数据给主进程
    int pipefd[2];
    //管道数组2,用于主进程分别发数据给子进程
    int pipearr[3][5];

    pid_t pid;
    char buff[20];
    int i;

    //创建管道
    if (0 > pipe(pipefd))
    {
        printf("Create Pipe Error!\n");
    }
    for (i = 0; i < 3; i++)
    {
        if (0 > pipe(pipearr[i]))
        {
            printf("Create Pipe Error!\n");
        }
    }

    //创建3个子进程
    for (i = 0; i < 3; i++)
    {
        pid = fork();

        //创建子进程失败
        if ((pid_t)0 > pid)
        {
            printf("Fork Error!\n");
            return 3;
        }

        //子进程逻辑
        if ((pid_t)0 == pid)
        {
            //发送格式化数据给主进程
            FILE *f;
            f = fdopen(pipefd[1], "w");
            fprintf(f, "I am %d\n", getpid());
            fclose(f);

            //接收父进程发过来的数据
            read(pipearr[i][0], buff, 20);
            printf("MyPid:%d, Message:%s", getpid(), buff);

            //完成后及时退出循环，继续循环会出大问题，和fork的运行逻辑有关！
            break;
        }
    }

    //主进程逻辑
    if ((pid_t)0 < pid)
    {
        //循环接收所有子进程发过来的数据，并且返回数据给子进程
        for (i = 0; i < 3; i++)
        {
            //接收子进程发来的数据
            read(pipefd[0], buff, 20);
            printf("MyPid:%d, Message:%s", getpid(), buff);

            //发送数据给子进程
            write(pipearr[i][6], "Hello My Son\n", 14);
        }
        sleep(3);
    }

    return 0;
}
