/* Compile with:
 * 	gcc tilesrv.c \
 * 		`pkg-config vips --cflags --libs` \
 * 		-lfcgi -luriparser \
 * 		-o tilesrv.fcgi
 *
 * Then copy to:
 *	/usr/local/httpd/fcgi-bin
 *
 * You may need to create the logfile, eg.
 *
 * 	touch /tmp/tilesrv.log
 * 	chmod ugo+rw /tmp/tilesrv.log
 *
 * Sample fcgi.conf:

<IfModule mod_fastcgi.c>
  FastCgiIpcDir /var/lib/apache2/fastcgi

  # Create a directory for the tilesrv binary
  ScriptAlias /fcgi-bin/ "/usr/local/httpd/fcgi-bin/"

  # Set the options on that directory
  <Directory "/usr/local/httpd/fcgi-bin">
     AllowOverride None
     Options None
     Order allow,deny
     Allow from all
  </Directory>

  # Set the module handler
  AddHandler fastcgi-script fcg fcgi fpl

  # Initialise some variables for the FCGI server
  FastCgiServer /usr/local/httpd/fcgi-bin/tilesrv.fcgi \
    -initial-env LOGFILE=/tmp/tilesrv.log 
  
</IfModule>
 
 */

#define VERSION "0.1"
#define PACKAGE "tilesrv"

/* No i18n for now.
 */
#define N_(A) (A)
#define _(A) (A)
#define GETTEXT_PACKAGE PACKAGE
#include <libintl.h>
#include <locale.h>

#include <stdio.h>
#include <stdarg.h>

/* apt-get install fastcgi 
 */
#include <fcgiapp.h>

/* apt-get install uriparser 
 */
#include <uriparser/Uri.h>

#include <vips/vips.h>

#define TILE_SIZE (128)

char *option_listen_socket = NULL;
char *option_log_filename = "/tmp/tilesrv.log";
gboolean option_version = FALSE;
FILE *log_fp = NULL;

static GOptionEntry option_list[] = {
	{ "bind", 'b', 0, G_OPTION_ARG_FILENAME, &option_listen_socket, 
		N_( "listen on SOCKET" ), 
		N_( "SOCKET" ) },
	{ "log", 'l', 0, G_OPTION_ARG_FILENAME, &option_log_filename, 
		N_( "log to FILENAME" ), 
		N_( "FILENAME" ) },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &option_version, 
		N_( "print version" ), NULL },
	{ NULL }
};

static void lg( const char *fmt, ... )
	__attribute__((format(printf, 1, 2)));

static void
lg( const char *fmt, ... )
{
	if( log_fp ) {
		va_list ap;        
		
		va_start( ap, fmt );
		vfprintf( log_fp, fmt, ap );
		va_end( ap );
		fflush( log_fp ); 
	}
}

static int
serve_tile( FCGX_Stream *out, const char *image, int level, int x, int y )
{
	VipsImage *vim;
	VipsImage *tile;
	void *buf;
	size_t len;

	if( vips_openslideload( image, &vim, "level", level, NULL ) ||
		vips_extract_area( vim, &tile, 
			x * TILE_SIZE, y * TILE_SIZE,
			TILE_SIZE, TILE_SIZE, NULL ) ||
		vips_jpegsave_buffer( tile, &buf, &len, "Q", 50, NULL ) )
		return( -1 ); 

	FCGX_FPrintF( out, "Content-type: image/jpeg\r\n\r\n");
	FCGX_PutStr( buf, len, out );

	g_free( buf ); 

	lg( "success: read tile %d x %d from level %d of image %s\n", 
			x, y, level, image ); 

	return( 0 );
}

static int
process_request( FCGX_Stream *out, const char *image, const char *path )
{
	char *level_str;
	char *tile_str;
	int x, y;
	int level;

	lg( "processing image = %s, path = %s\n", image, path );

	/* Tiles are named as eg. "6/3_7.jpg", meaning level 6, tile 3 x 7.
	 */
	tile_str = g_path_get_basename( path );
	level_str = g_path_get_dirname( path );

	if( sscanf( tile_str, "%d_%d.jpg", &x, &y ) != 2 ||
		sscanf( level_str, "%d", &level ) != 1 ) {
		lg( "unable to parse x_y.jpg from %s\n", tile_str );
		lg( "unable to parse level from %s\n", level_str );
		g_free( tile_str );
		g_free( level_str );
		return( -1 );
	}

	if( x < 0 || x > 100000 ||
		y < 0 || y > 100000 ||
		level < 0 || level > 100000 ) {
		lg( "level/x/y out of range\n" ); 
		g_free( tile_str );
		g_free( level_str );
		return( -1 );
	}

	if( serve_tile( out, image, level, x, y ) ) {
		g_free( tile_str );
		g_free( level_str );
		return( -1 );
	}

	g_free( tile_str );
	g_free( level_str );

	return( 0 );
}

static int
handle_query( FCGX_Stream *out, const char *query )
{
	UriUriA uri;
	UriQueryListA *query_list;
	UriQueryListA *p;
	int count;
	const char *image;
	const char *path;

	if( uriDissectQueryMallocA( &query_list, &count, 
		query, query + strlen( query ) ) != URI_SUCCESS ) {
		vips_error( PACKAGE, "unable to parse query" ); 
		return( -1 );
	}

	image = NULL;
	path = NULL;

	for( p = query_list; p; p = p->next ) { 
		if( p->value &&
			strcmp( p->key, "image" ) == 0 ) 
			image = p->value;
		if( p->value &&
			strcmp( p->key, "path" ) == 0 ) 
			path = p->value;
	}

	if( !image || 
		!path ) {
		vips_error( PACKAGE, "unable to parse query" ); 
		uriFreeQueryListA( query_list );
		return( -1 );
	}

	if( process_request( out, image, path ) ) {
		uriFreeQueryListA( query_list );
		return( -1 ); 
	}

	uriFreeQueryListA( query_list );

	return( 0 );
}

int
main( int argc, char **argv )
{
	GOptionContext *context;
	GOptionGroup *main_group;
	GOptionGroup *group;
	FCGX_Request request;

	int listen_socket = 0;

	GError *error = NULL;

	if( vips_init( argv[0] ) )
	        vips_error_exit( NULL );
	textdomain( GETTEXT_PACKAGE );
	setlocale( LC_ALL, "" );

	context = g_option_context_new( _( "[OPTIONS] - fcgi tile server" ) );

	main_group = g_option_group_new( NULL, NULL, NULL, NULL, NULL );
	g_option_group_add_entries( main_group, option_list );
	g_option_group_set_translation_domain( main_group, GETTEXT_PACKAGE );
	g_option_context_set_main_group( context, main_group );

	/* Add the libvips options too.
	 */
	g_option_context_add_group( context, vips_get_option_group() );

	if( !g_option_context_parse( context, &argc, &argv, &error ) ) {
		if( error ) {
			fprintf( stderr, "%s\n", error->message );
			g_error_free( error );
		}

		vips_error_exit( NULL );
	}

	if( option_version )
		vips_error_exit( VERSION ); 

	if( FCGX_Init() )
		vips_error_exit( _( "unable to init fcgx" ) ); 

	if( option_log_filename &&
		!(log_fp = fopen( option_log_filename, "a" )) ) 
		vips_error_exit( _( "unable to append to %s" ), 
			option_log_filename ); 

	if( option_listen_socket ) {
		listen_socket = FCGX_OpenSocket( option_listen_socket, 10 );
		if( listen_socket < 0 ) 
			vips_error_exit( "unable to open socket %s", 
				option_listen_socket );
	}

	if( FCGX_InitRequest( &request, listen_socket, 0 ) )
		vips_error_exit( "unable to init FCGX request" ); 

	while( FCGX_Accept_r( &request ) >= 0 ) {
		char *query = FCGX_GetParam( "QUERY_STRING", request.envp );

		lg( "seen QUERY_STRING: %s\n", query );

		if( handle_query( request.out, query ) ) {
			/* Don't give any details here for security.
			 */
			FCGX_FPrintF( request.out, 
				"Content-type: text/plain\r\n\r\n");
			FCGX_FPrintF( request.out, "error processing query\n" );
			FCGX_FPrintF( request.out, "check server logs\n" );

			lg( "error processing QUERY_STRING: %s\n", query );
			lg( "vips error: %s\n", vips_error_buffer() );

			FCGX_FPrintF( request.err, 
				"processing %s; vips error: %s\n", 
				query, 
				vips_error_buffer() ); 

			vips_error_clear();
		}
	}

	FCGX_Finish_r( &request ); 

	return 0;
}
