#define _GNU_SOURCEulimit -n 10
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include "sock.h"
#include "common.h"
#include "log.h"

#include "parser.h"

//hola

#define MAX_EVENTS 10

typedef struct arg_struct {
	int epoll;
  int text_sock;
  int bin_sock;
}args;

typedef args *argsPuntero;

/* Macro interna */
#define READ(fd, buf, n) ({						\
	int rc = read(fd, buf, n);					\
	if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))	\
		return 0;						\
	if (rc <= 0)							\
		return -1;						\
	rc; })

/* 0: todo ok, continua. -1 errores */
int text_consume(struct eventloop_data *evd, char buf[2024], int fd, int blen)
{
	int rem = sizeof *buf - blen;
	int nread;
	char *p, *p0 = buf;
	int nlen = blen;

	while (blen < 2024) {
		nread = READ(fd, buf + blen, 1);
		blen += nread;
	}

	nlen = blen;
	//printf("buf: %s, p0: %s\n", buf, p0);

		/* Para cada \n, procesar, y avanzar punteros */
		while ((p = memchr(p0, '\n', nlen)) != NULL) {
			/* Mensaje completo */
			int len = p - p0;
			*p++ = 0;
			log(3, "full command: <%s>", p0);
			char *toks[3]= {NULL};
			int lens[3] = {0};
			int ntok;
			ntok = text_parser(buf,toks,lens);

			/*text_handle(evd, p0, len, ....);
				Acá podríamos ver que hacemos con los tokens encontrados:
				toks[0] tendrá PUT, GET, DEL, ó STATS si se ingresó un comando válido.
			*/

			nlen -= len + 1;
			p0 = p;
		}

		/* Si consumimos algo, mover */
		if (p0 != buf) {
			memmove(buf, p0, nlen);
			blen = nlen;
		}

	return 0;
}

void limit_mem()
{
	/*Implementar*/
}

void handle_signals()
{
    /*Capturar y manejar  SIGPIPE */
    //SIGPIPE es la señal que se le envia a un proceso cuando este intenta escribir en un pipe cuyo extremo de lectura ha sido cerrado. La accion por defecto es terminar el proceso, pero nosotros queremos evitar esto, queremos que en lugar de terminar el proceso ignore la señal.
    signal(SIGPIPE, SIG_IGN);
}

int setnonblocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);

    if (flags == -1)
      return -1;  // error

    int result = fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    return result;
}

void *wait_for_clients(void *args)
{
	struct epoll_event events[MAX_EVENTS], ev_conn, ev_mod;
	int nfds, conn_sock;
	char buf[2024];
	struct eventloop_data *evd = malloc(sizeof(struct eventloop_data));

	argsPuntero argumentos = *(argsPuntero *)args;
	int epfd = argumentos->epoll;
	int text_sock = argumentos->text_sock;
	int bin_sock = argumentos->bin_sock;

	evd->epfd = epfd;
	evd->id = pthread_self();
	evd->n_proc = getpid();

	//bucle para que la instancia epfd se mantenga indefinidamente esperando nuevos clientes
	for (;;) {
		printf(":D\n");
		//espera indefinidamente que suceda algun evento en algun fd de la instancia epfd
		if ((nfds = epoll_wait(epfd, events, MAX_EVENTS, -1)) == -1) { //en events se guardara informacion de los fd que esten listos y tengan eventos disponibles
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}

		//events guarda informacion sobre los fd de la instancia epfd que estan en estado listo y sobre los eventos que han ocurrido en dichos file descriptors
		//nfds es la cantidad de file descriptors que estan en estado listo
		for (int n = 0; n < nfds; ++n) {
			if (events[n].data.fd == text_sock || events[n].data.fd == bin_sock) { //chequeamos que socket ha disparado un evento
				if ((conn_sock = accept(events[n].data.fd, NULL, NULL)) == -1) { //establecemos una conexion con el socket
					quit("accept");
					exit(EXIT_FAILURE);
				}

				//hacer que conn_sock sea no bloqueante para que no trabe el thread cuando no tenga nada que leer
				if (setnonblocking(conn_sock) == -1) {
					quit("error O_NONBLOCK");
				}

				ev_conn.events = EPOLLIN | EPOLLONESHOT;
				ev_conn.data.fd = conn_sock;

				if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn_sock, &ev_conn) == -1) { //agregamos la conexion a la instancia epfd
					perror("epoll_ctl: conn_sock");
					exit(EXIT_FAILURE);
				}

				ev_mod.events = EPOLLIN | EPOLLONESHOT;
				ev_mod.data.fd = events[n].data.fd;

				if (epoll_ctl(epfd, EPOLL_CTL_MOD, events[n].data.fd, &ev_mod) == -1) {
					perror("epoll_ctl: conn_sock");
					exit(EXIT_FAILURE);
				}

				//luego de haber agregado a la instancia las conexiones a los sockets volvera a epoll_wait para esperar nuevas conexiones o eventos disparados por conexiones ya existentes, en cuyo caso se ejecutara la funcion handle_conn
				//¿pero que pasa si mientras se esta procesando un evento sucede otro? Se guarda en events
				//tener en cuenta que cuando la conexion finalice hay que liberar memoria de ev_conn

			} else {
				//si no termino de consumir el buffer, se seguiran disparando eventos (va a aseguir de largo en epoll_wait)
				text_consume(evd, buf, events[n].data.fd, 0);
				//aca va text_consume o bin_consume ¿como me doy cuenta a que modo pertenece la conexion?
				//printf("fd: %li, %s", pthread_self(), buf);

				//esta parte no va si la conexion debe terminar
				if (setnonblocking(events[n].data.fd) == -1) {
					quit("error O_NONBLOCK");
				}

				ev_mod.events = EPOLLIN | EPOLLONESHOT;
				ev_mod.data.fd = events[n].data.fd;

				if (epoll_ctl(epfd, EPOLL_CTL_MOD, events[n].data.fd, &ev_mod) == -1) {
					perror("epoll_ctl: conn_sock");
					exit(EXIT_FAILURE);
				}
				//---------------------------------------------
			}
		}
	}

	/*Configurar Epoll

	En algún momento al manejar eventos de tipo EPOLLIN de un cliente
	en modo texto invocaremos a text_consume:
	int rc;
	rc = text_consume(evd, buff, fd, blen);
	y algo parecido habrá que hacer al momento al manejar eventos de tipo
	EPOLLIN de un cliente en modo binario.

	*/
}

void server(int text_sock, int bin_sock)
{
	int epfd;
	if ((epfd = epoll_create1(0)) == -1) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	//incluimos EPOLLONESHOT para que luego de dispararse un evento el fd quede deshabilitado y asi solo un thread capturara el evento en el epoll_wait
	//pero luego debera rearmarse el fd  mediante epoll_ctl con EPOLL_CTL_MOD
	struct epoll_event ev_text;
	ev_text.events = EPOLLIN | EPOLLONESHOT; //indica que el data.fd esta disponible para realizar operacion de lectura
	ev_text.data.fd = text_sock;

	struct epoll_event ev_bin;
	ev_bin.events = EPOLLIN | EPOLLONESHOT; //indica que el data.fd esta disponible para realizar operacion de lectura
	ev_bin.data.fd = bin_sock;

	//agregamos text_sock a la lista de files descriptors de epfd, que sera notificado cada vez que ocurra un evento de ev_text (por ejemplo cuando text_sock este disponible para leer)
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, text_sock, &ev_text) == -1) {
		perror("epoll_ctl: listen_sock");
		exit(EXIT_FAILURE);
	}

	//agregamos bin_sock a la lista de files descriptors de epfd, que sera notificado cada vez que ocurra un evento de ev_bin
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, bin_sock, &ev_bin) == -1) {
		perror("epoll_ctl: listen_sock");
		exit(EXIT_FAILURE);
	}

	long nt = sysconf(_SC_NPROCESSORS_ONLN);
	pthread_t threads[nt];

	args *args = malloc(sizeof(struct arg_struct));
	args->epoll = epfd;
	args->text_sock = text_sock;
	args->bin_sock = bin_sock;

	//la funcion wait_for_clients se ejecutara en cada thread
	for(size_t i = 0; i < nt-1; i++) {
		pthread_create(&threads[i], NULL, wait_for_clients, (void *)&args);
	}
	for(size_t i = 0; i < nt-1; i++) {
		pthread_join(threads[i], NULL);
	}
}

int main(int argc, char **argv)
{
	int text_sock, bin_sock;

	//__loglevel = 2;

	handle_signals();

	/*Función que limita la memoria*/
	limit_mem();

	text_sock = mk_tcp_sock(mc_lport_text);
	if (text_sock < 0)
		quit("mk_tcp_sock.text");

	bin_sock = mk_tcp_sock(mc_lport_bin);
	if (bin_sock < 0)
		quit("mk_tcp_sock.bin");


	/*Inicializar la tabla hash, con una dimensión apropiada*/
	/* 1 millón de entradas, por ejemplo*/
	/* .....*/

	/*Iniciar el servidor*/
	server(text_sock, bin_sock);

	return 0;
}
