/* simple deepzoom fastcgi tile server
 */

#define VERSION "0.1"
#define PACKAGE "tilesrv"

/*
#define DEBUG
 */

/* No i18n for now.
 */
#define N_(A) (A)
#define _(A) (A)
#define GETTEXT_PACKAGE PACKAGE
//#include <libintl.h>
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

#ifdef DEBUG

int loops = 2;
#define FCGX_Accept_r(R) ((R)->out = fopen("output", "w"), (R)->err = stderr, loops--)
#define FCGX_GetParam(A, B) \
	("filename=/home/john/pics/044TracheaEsophCat.svs&path=4/0_0.jpeg")
#define FCGX_FPrintF fprintf
#define FCGX_PutStr( B, L, F ) fwrite( B, L, 1, F )
#define FCGX_InitRequest(A, B, C) (0)
#define FCGX_Finish_r(C) (0)
#define FCGX_Stream FILE
#define FCGX_Request DebugRequest

typedef struct _DebugRequest {
	FILE *out;
	FILE *err;
	char **envp;
} DebugRequest;

#endif

#define TILE_SIZE (256)
#define MAX_PYRAMIDS (10)

/* A slice in the pyramid.
 *
 * openslide levels get slotted in where we have them, we downsample to make
 * the missing slices. 
 */
typedef struct _Slice {
	char *filename;

	/* The image for this slice.
	 */
	VipsImage *image;

	int width;
	int height; 
	int sub;			/* Subsample factor for this slice */
	int n;				/* Slice number ... 0 for smallest */

	/* Up and down the pyramid. above is the larger image, we shrink as we
	 * go down. 
	 */
	struct _Slice *below;
	struct _Slice *above;
} Slice;

char *option_listen_socket = NULL;
char *option_log_filename = "/tmp/tilesrv.log";
gboolean option_version = FALSE;
FILE *log_fp = NULL;

static GOptionEntry option_list[] = {
	{ "bind", 'b', 0, G_OPTION_ARG_FILENAME, &option_listen_socket, 
		N_( "listen on SOCKET" ), 
		N_( "SOCKET" ) },
	{ "log", 'l', 0, G_OPTION_ARG_FILENAME, &option_log_filename, 
		N_( "log to FILENAME, - for no log" ), 
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

#ifdef DEBUG
		va_start( ap, fmt );
		vprintf( fmt, ap );
		va_end( ap );
#endif
	}
}

/* Free a pyramid.
 */
static void *
slice_free( Slice *slice )
{
	VIPS_FREE( slice->filename ); 
	VIPS_FREEF( g_object_unref, slice->image );
	VIPS_FREEF( slice_free, slice->below ); 
	VIPS_FREE( slice ); 

	return( NULL );
}

/* Build a pyramid. 
 */
static Slice *
pyramid_build( Slice *above, int width, int height )
{
	Slice *slice = VIPS_NEW( NULL, Slice );

	slice->filename = NULL;
	slice->image = NULL;
	slice->width = width;
	slice->height = height;

	if( !above )
		/* Top of pyramid.
		 */
		slice->sub = 1;	
	else
		slice->sub = above->sub * 2;

	slice->below = NULL;
	slice->above = above;

	if( width > 1 || 
		height > 1 ) {
		/* Round up, so eg. a 5 pixel wide image becomes 3 a slice
		 * down.
		 */
		slice->below = pyramid_build( slice, 
			(width + 1) / 2, (height + 1) / 2 );
		slice->n = slice->below->n + 1;
	}
	else
		slice->n = 0;

#ifdef DEBUG
	printf( "pyramid_build:\n" );
	printf( "\tn = %d\n", slice->n );
	printf( "\twidth = %d, height = %d\n", width, height );
#endif

	return( slice );
}

/* Open a level in an openslide image. Drop the alpha while we're at it.
 */
static VipsImage *
open_image( const char *filename, int level )
{
	VipsImage *t[2];

	if( vips_openslideload( filename, &t[0], "level", level, NULL ) )
		return( NULL );
	if( vips_extract_band( t[0], &t[1], 0, "n", 3, NULL ) ) {
		g_object_unref( t[0] );
		return( NULL );
	}
	g_object_unref( t[0] );

	return( t[1] );
}

/* Insert an image into a slice in the pyramid. We may need to expand 
 * the image slightly for rounding.
 */
static int
pyramid_insert_slice( Slice *slice, VipsImage *image )
{
	VipsImage *in;
	VipsImage *x;
	int max_tiles;

#ifdef DEBUG
	printf( "pyramid_insert_slice:\n" );
	printf( "\timage->Xsize = %d\n", image->Xsize );
	printf( "\timage->Xsize = %d\n", image->Ysize );
	printf( "\twidth = %d, height = %d\n", slice->width, slice->height );
#endif

	in = image;
	g_object_ref( in );

	if( vips_embed( in, &x, 0, 0, slice->width, slice->height,
		"extend", VIPS_EXTEND_COPY, NULL ) ) {
		g_object_unref( in ); 
		return( -1 ); 
	}
	g_object_unref( in ); 
	in = x;

	/* We cache enough tiles in each layer to be able to paint a 1920 x
	 * 1080 desktop, plus a bit.
	 */
	max_tiles = (3 + 1920 / TILE_SIZE) * (3 + 1080 / TILE_SIZE);

	if( vips_tilecache( in, &x, 
		"tile_width", TILE_SIZE, 
		"tile_height", TILE_SIZE, 
		"max_tiles", max_tiles, 
		"persistent", TRUE, 
		NULL ) ) {
		g_object_unref( in ); 
		return( -1 ); 
	}
	g_object_unref( in ); 
	in = x;

	slice->image = in;

	return( 0 );
}

/* Search the pyramid for the right slice for an openslide level.
 */
static Slice *
pyramid_find_slice( Slice *pyramid, VipsImage *level )
{
	char *str;
	int slide_level;
	char field_name[256];
	int downsample;
	Slice *p;

	if( vips_image_get_int( level, "slide-level", &slide_level ) ) 
		return( NULL ); 
	vips_snprintf( field_name, 256, 
		"openslide.level[%d].downsample", slide_level ); 
	if( vips_image_get_string( level, field_name, &str ) ) 
		return( NULL );
	/* downsample can be eg. "4.000069474", we just want the int part.
	 */
	downsample = (int) (atof( str ));

	for( p = pyramid; p; p = p->below )
		if( p->sub == downsample )
			break;
	if( !p ) {
		vips_error( PACKAGE, 
			"downsample %d not in pyramid", downsample );
		return( NULL );
	}

	return( p );
}

/* Make a slice, if it doesn't exist. Downsample the slice above.
 */
static int
pyramid_create_slice( Slice *slice )
{
	VipsImage *in;
	VipsImage *x;

	/* Shrink will complain if the source has only 1 row or column. Expand
	 * in this case.
	 */
	in = slice->above->image;
	g_object_ref( in ); 

	if( in->Xsize == 1 ||
		in->Ysize == 1 ) {
		if( vips_embed( in, &x, 0, 0, 
			VIPS_MIN( 2, in->Xsize + 1 ), 
			VIPS_MIN( 2, in->Ysize + 1 ), 
			"extend", VIPS_EXTEND_COPY, NULL ) ) {
			g_object_unref( in ); 
			return( -1 );
		}
		g_object_unref( in ); 
		in = x;
	}

	if( vips_shrink( in, &x, 2, 2, NULL ) ) {
		g_object_unref( in ); 
		return( -1 );
	}
	g_object_unref( in ); 
	in = x;

	if( pyramid_insert_slice( slice, in ) ) {
		g_object_unref( in );
		return( -1 );
	}
	g_object_unref( in );

	return( 0 );
}

/* Make a pyramid from an openslide file. 
 */
static Slice *
pyramid_from_file( const char *filename )
{
	VipsImage *image;
	Slice *pyramid;
	char *str;
	int n_levels;
	int i;
	Slice *p;

#ifdef DEBUG
	printf( "pyramid_from_file: %s\n", filename );
#endif

	if( !(image = open_image( filename, 0 )) ) 
		return( NULL );
	pyramid = pyramid_build( NULL, image->Xsize, image->Ysize );
	pyramid->filename = g_strdup( filename );

	if( vips_image_get_string( image, "openslide.level-count", &str ) ) {
		slice_free( pyramid );
		return( NULL ); 
	}
	n_levels = atoi( str );
	if( n_levels < 1 ||
		n_levels > 100 ) {
		vips_error( PACKAGE, "bad levels in pyramid" );
		slice_free( pyramid );
		return( NULL ); 
	}

	if( pyramid_insert_slice( pyramid, image ) ) {
		slice_free( pyramid );
		return( NULL ); 
	}
	g_object_unref( image ); 

	for( i = 1; i < n_levels; i++ ) {
		VipsImage *level;

#ifdef DEBUG
		printf( "pyramid_from_file: inserting level %d\n", i );
#endif

		if( !(level = open_image( filename, i )) ) {
			slice_free( pyramid );
			return( NULL ); 
		}

		if( !(p = pyramid_find_slice( pyramid, level )) ||
			pyramid_insert_slice( p, level ) ) { 
			g_object_unref( level ); 
			slice_free( pyramid );
			return( NULL ); 
		}
		g_object_unref( level ); 
	}

	for( p = pyramid; p; p = p->below ) 
		if( !p->image ) {
#ifdef DEBUG
			printf( "pyramid_from_file: creating level %d\n", 
				p->n );
#endif

			if( pyramid_create_slice( p ) ) {
				slice_free( pyramid );
				return( NULL ); 
			}
		}

	return( pyramid );
}

static GSList *current_pyramids = NULL; 

static void
pyramid_shutdown( void )
{
	vips_slist_map2( current_pyramids, 
		(VipsSListMap2Fn) slice_free, NULL, NULL ); 
	g_slist_free( current_pyramids ); 
}

static void *
pyramid_find_sub( Slice *pyramid, const char *filename )
{
	if( strcmp( pyramid->filename, filename ) == 0 )
		return( pyramid );

	return( NULL );
}

static Slice *
pyramid_find( const char *filename )
{
	Slice *pyramid;

	if( (pyramid = (Slice *) vips_slist_map2( current_pyramids,
		(VipsSListMap2Fn) pyramid_find_sub, 
			(char *) filename, NULL )) ) {
		current_pyramids = g_slist_remove( current_pyramids, pyramid );
		current_pyramids = g_slist_append( current_pyramids, pyramid );
		return( pyramid );
	}

	if( g_slist_length( current_pyramids ) > MAX_PYRAMIDS ) {
		pyramid = (Slice *) current_pyramids->data; 
		current_pyramids = g_slist_remove( current_pyramids, pyramid );
		slice_free( pyramid );
	}

	if( !(pyramid = pyramid_from_file( filename )) )
		return( NULL );

	current_pyramids = g_slist_append( current_pyramids, pyramid );

	return( pyramid );
}

static int
serve_tile( FCGX_Stream *out, const char *filename, int n, int x, int y )
{
	Slice *pyramid;
	Slice *p;
	VipsImage *tile;
	void *buf;
	size_t len;

	if( !(pyramid = pyramid_find( filename )) )
		return( -1 );

	for( p = pyramid; p; p = p->below )
		if( p->n == n )
			break;
	if( !p ) {
		vips_error( PACKAGE, "layer not in pyramid" ); 
		return( -1 );
	}

	if( vips_extract_area( p->image, &tile, 
		x * TILE_SIZE, y * TILE_SIZE,
		VIPS_MIN( p->image->Xsize - x * TILE_SIZE, TILE_SIZE ),
		VIPS_MIN( p->image->Ysize - y * TILE_SIZE, TILE_SIZE ),
		NULL ) ) 
		return( -1 );
	
	if( vips_jpegsave_buffer( tile, &buf, &len, "Q", 50, NULL ) ) {
		g_object_unref( tile );
		return( -1 ); 
	}

	FCGX_FPrintF( out, "Content-length: %zd\r\n", len );
	FCGX_FPrintF( out, "Content-type: image/jpeg\r\n");
	FCGX_FPrintF( out, "\r\n");
	FCGX_PutStr( buf, len, out );

	g_free( buf ); 
	g_object_unref( tile ); 

	lg( "success: read tile %d x %d from level %d of image %s\n", 
		x, y, n, filename ); 

	return( 0 );
}

static int
process_request( FCGX_Stream *out, const char *filename, const char *path )
{
	char *level_str;
	char *tile_str;
	int x, y;
	int n;

	lg( "processing filename = %s, path = %s\n", filename, path );

	/* Tiles are named as eg. "6/3_7.jpeg", meaning level 6, tile 3 x 7.
	 */
	tile_str = g_path_get_basename( path );
	level_str = g_path_get_dirname( path );

	if( sscanf( tile_str, "%d_%d.jpeg", &x, &y ) != 2 ||
		sscanf( level_str, "%d", &n ) != 1 ) {
		lg( "unable to parse x_y.jpeg from %s\n", tile_str );
		lg( "unable to parse level from %s\n", level_str );
		g_free( tile_str );
		g_free( level_str );
		return( -1 );
	}

	if( x < 0 || x > 100000 ||
		y < 0 || y > 100000 ||
		n < 0 || n > 100000 ) {
		lg( "n/x/y out of range\n" ); 
		g_free( tile_str );
		g_free( level_str );
		return( -1 );
	}

	if( serve_tile( out, filename, n, x, y ) ) {
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
	UriQueryListA *query_list;
	UriQueryListA *p;
	int count;
	const char *filename;
	const char *path;

	if( uriDissectQueryMallocA( &query_list, &count, 
		query, query + strlen( query ) ) != URI_SUCCESS ) {
		vips_error( PACKAGE, "unable to parse query" ); 
		return( -1 );
	}

	filename = NULL;
	path = NULL;

	for( p = query_list; p; p = p->next ) { 
		if( p->value &&
			strcmp( p->key, "filename" ) == 0 ) 
			filename = p->value;
		if( p->value &&
			strcmp( p->key, "path" ) == 0 ) 
			path = p->value;
	}

	if( !filename || 
		!path ) {
		vips_error( PACKAGE, "unable to parse query" ); 
		uriFreeQueryListA( query_list );
		return( -1 );
	}

	if( process_request( out, filename, path ) ) {
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
	FCGX_Request request;

	int listen_socket = 0;
	const char *str;

#ifdef DEBUG
	GTimer *query_timer = NULL;
#endif

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

	if( (str = g_getenv( "TILESRV_LOGFILE" )) )
		option_log_filename = g_strdup( str ); 
	if( (str = g_getenv( "TILESRV_LISTEN" )) )
		option_listen_socket = g_strdup( str ); 

	if( option_version )
		vips_error_exit( VERSION ); 

	if( FCGX_Init() )
		vips_error_exit( _( "unable to init fcgx" ) ); 

	if( option_log_filename &&
		strcmp( option_log_filename, "-" ) != 0 && 
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

#ifdef DEBUG
		if( !query_timer ) 
			query_timer = g_timer_new();
		g_timer_reset( query_timer );
#endif

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

#ifdef DEBUG
		printf( "query took: %gs\n",  
			g_timer_elapsed( query_timer, NULL ) );
#endif
	}

	FCGX_Finish_r( &request ); 

	pyramid_shutdown();

	return 0;
}
