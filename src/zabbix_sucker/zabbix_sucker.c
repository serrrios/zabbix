#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>

#include <signal.h>
#include <errno.h>

#include <time.h>

#include <syslog.h>

#include "common.h"
#include "db.h"
#include "functions.h"

void	signal_handler( int sig )
{
	if( SIGALRM == sig )
	{
		signal( SIGALRM, signal_handler );
 
		syslog( LOG_WARNING, "Timeout while executing operation." );
	}
 
	if( SIGQUIT == sig || SIGINT == sig || SIGTERM == sig )
	{
		syslog( LOG_ERR, "\nGot QUIT or INT or TERM signal. Exiting..." );
		exit( FAIL );
	}
 
	return;
}

void	daemon_init(void)
{
	int	i;
	pid_t	pid;

	if( (pid = fork()) != 0 )
	{
		exit( 0 );
	}
	setsid();

	signal( SIGHUP, SIG_IGN );

	if( (pid = fork()) !=0 )
	{
		exit( 0 );
	}

	chdir("/");

	umask(0);

	for(i=0;i<MAXFD;i++)
	{
		close(i);
	}
}

int	get_value(double *result,char *key,char *host,int port)
{
	int	s;
	int	i;
	char	c[1024];
	char	*e;
	void	*sigfunc;

	struct hostent *hp;

	struct sockaddr_in myaddr_in;
	struct sockaddr_in servaddr_in;

	syslog( LOG_DEBUG, "%10s%25s\t", host, key );

	servaddr_in.sin_family=AF_INET;
	hp=gethostbyname(host);

	if(hp==NULL)
	{
		syslog( LOG_WARNING, "Problem with gethostbyname" );
		return	FAIL;
	}

	servaddr_in.sin_addr.s_addr=((struct in_addr *)(hp->h_addr))->s_addr;

	servaddr_in.sin_port=htons(port);

	s=socket(AF_INET,SOCK_STREAM,0);
	if(s==0)
	{
		syslog( LOG_WARNING, "Problem with socket" );
		return	FAIL;
	}
 
	myaddr_in.sin_family = AF_INET;
	myaddr_in.sin_port=0;
	myaddr_in.sin_addr.s_addr=INADDR_ANY;

	sigfunc = signal( SIGALRM, signal_handler );

	alarm(SUCKER_TIMEOUT);

	if( connect(s,(struct sockaddr *)&servaddr_in,sizeof(struct sockaddr_in)) == -1 )
	{
		syslog( LOG_WARNING, "Problem with connect" );
		close(s);
		return	FAIL;
	}
	alarm(0);
	signal( SIGALRM, sigfunc );

	sprintf(c,"%s\n",key);
	if( sendto(s,c,strlen(c),0,(struct sockaddr *)&servaddr_in,sizeof(struct sockaddr_in)) == -1 )
	{
		syslog(LOG_WARNING, "Problem with sendto" );
		close(s);
		return	FAIL;
	} 
	i=sizeof(struct sockaddr_in);

	sigfunc = signal( SIGALRM, signal_handler );
	alarm(SUCKER_TIMEOUT);

	i=recvfrom(s,c,1023,0,(struct sockaddr *)&servaddr_in,&i);
	if(i==-1)
	{
		syslog( LOG_WARNING, "Problem with recvfrom [%d]",errno );
		close(s);
		return	FAIL;
	}
	alarm(0);
	signal( SIGALRM, sigfunc );
 
	if( close(s)!=0 )
	{
		syslog(LOG_WARNING, "Problem with close" );
	}
	c[i-1]=0;

	syslog(LOG_DEBUG, "Got string:%10s", c );
	*result=strtod(c,&e);

	if( (*result==0) && (c==e) )
	{
		return	FAIL;
	}
	if( *result<0 )
	{
		if( *result == NOTSUPPORTED)
		{
			return SUCCEED;
		}
		else
		{
			return	FAIL;
		}
	}
	return SUCCEED;
}

int get_minnextcheck(void)
{
	char		c[1024];

	DB_RESULT	*result;
	DB_ROW		row;

	int		res;

	sprintf(c,"select min(nextcheck) from items i,hosts h where i.status=0 and h.status=0 and h.hostid=i.hostid and i.status=0");
	DBexecute(c);

	result = DBget_result();
	if(result==NULL)
	{
		syslog(LOG_DEBUG, "No items to update for minnextcheck.");
		DBfree_result(result);
		return FAIL; 
	}
	if(DBnum_rows(result)==0)
	{
		syslog( LOG_DEBUG, "No items to update for minnextcheck.");
		DBfree_result(result);
		return	FAIL;
	}

	row = DBfetch_row(result);
	if( row[0] == NULL )
	{
		DBfree_result(result);
		return	FAIL;
	}

	res=atoi(row[0]);
	DBfree_result(result);

	return	res;
}

int get_values(void)
{
	double		value;
	char		c[1024];
	ITEM		item;
 
	DB_RESULT	*result;
	DB_ROW		row;

	sprintf(c,"select i.itemid,i.key_,h.host,h.port,i.delay,i.description,i.history,i.lastdelete from items i,hosts h where i.nextcheck<=unix_timestamp() and i.status=0 and h.status=0 and h.hostid=i.hostid order by i.nextcheck");
	DBexecute(c);

	result = DBget_result();
	if(result==NULL)
	{
		syslog( LOG_DEBUG, "No items to update.");
		DBfree_result(result);
		return SUCCEED; 
	}
	while ( (row = DBfetch_row(result)) != NULL )
	{
		item.itemid=atoi(row[0]);
		item.key=row[1];
		item.host=row[2];
		item.port=atoi(row[3]);
		item.delay=atoi(row[4]);
		item.description=row[5];
		item.history=atoi(row[6]);
		item.lastdelete=atoi(row[7]);
		item.shortname=row[8];

		if( get_value(&value,item.key,item.host,item.port) == SUCCEED )
		{
			if( value == NOTSUPPORTED)
			{
				sprintf(c,"update items set status=3 where itemid=%d",item.itemid);
				DBexecute(c);
			}
			else
			{
				sprintf(c,"insert into history (itemid,clock,value) values (%d,unix_timestamp(),%g)",item.itemid,value);
				DBexecute(c);

				sprintf(c,"update items set NextCheck=unix_timestamp()+%d,PrevValue=LastValue,LastValue=%f,LastClock=unix_timestamp() where ItemId=%d",item.delay,value,item.itemid);
				DBexecute(c);

				if( update_functions( item.itemid ) == FAIL)
				{
					syslog( LOG_WARNING, "Updating simple functions failed" );
				}
			}
		}
		else
		{
			syslog( LOG_WARNING, "Wrong value from host [HOST:%s KEY:%s VALUE:%f]", item.host, item.key, value );
			syslog( LOG_WARNING, "The value is not stored in database.");
		}

		if(item.lastdelete+3600<time(NULL))
		{
			sprintf	(c,"delete from history where ItemId=%d and Clock<unix_timestamp()-%d",item.itemid,item.history);
			DBexecute(c);
	
			sprintf(c,"update items set LastDelete=unix_timestamp() where ItemId=%d",item.itemid);
			DBexecute(c);
		}
	}
	DBfree_result(result);
	return SUCCEED;
}

int main_loop()
{
	time_t now;

	int	nextcheck,sleeptime;

	for(;;)
	{
		now=time(NULL);
		get_values();

		syslog( LOG_DEBUG, "Spent %d seconds while updating values", time(NULL)-now );

		nextcheck=get_minnextcheck();
		syslog( LOG_DEBUG, "Nextcheck:%d Time:%d", nextcheck,time(NULL) );

		if( FAIL == nextcheck)
		{
			sleeptime=SUCKER_DELAY;
		}
		else
		{
			sleeptime=nextcheck-time(NULL);
			if(sleeptime<0)
			{
				sleeptime=0;
			}
		}
		if(sleeptime>0)
		{
			syslog( LOG_DEBUG, "Sleeping for %d seconds", sleeptime );
			sleep( sleeptime );
		}
		else
		{
			syslog( LOG_DEBUG, "No sleeping" );
		}
	}
}

int main(int argc, char **argv)
{
	int 	ret;

	daemon_init();

	signal( SIGINT,  signal_handler );
	signal( SIGQUIT, signal_handler );
	signal( SIGTERM, signal_handler );


	openlog("zabbix_sucker",LOG_PID,LOG_USER);
//      ret=setlogmask(LOG_UPTO(LOG_DEBUG));
	ret=setlogmask(LOG_UPTO(LOG_WARNING));

	syslog( LOG_WARNING, "zabbix_sucker started");

	DBconnect();

	main_loop();

	return SUCCEED;
}
