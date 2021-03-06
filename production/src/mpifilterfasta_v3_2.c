// source code for filterfasta program.
// Eduardo Ponce
// 4/06/2014
//
// filterfasta is a program for parsing files in FASTA format which contain amino acid sequences of proteins/nucleotides. filterfasta expects a valid FASTA file, no validation on the format is performed.
//
// The first functionality of filterfasta is to extract sequences from a FASTA input file and write them in FASTA format to an output file. The filterfasta program uses command line options to allow the user to control which sequences to extract for output by specifying the maximum amount of sequences to extract, the exact length (or ranged lengths) of amino acids, the sequence annotation fields to maintain, and the maximum size in bytes allowed for the output file.
// 
// The second functionality of filterfasta is serve as pipeline program between BLAST and HMMER/MUSCLE.
// 	HMMER  --> filterfasta extracts sequences from a FASTA input file that appear as hits in a BLAST table file and writes them in FASTA format to an output file. The output file serve as input for HMMER program.
// 	MUSCLE --> (under development) filterfasta extracts sequences from a FASTA input file that appear as hits in a BLAST table file and writes them in FASTA format to multiple output files grouped by the hits' queries. The output files serve as input for MUSCLE program.


////////////////////////////////////////////////////////////////////////////////
//                              Header Files                                  //
////////////////////////////////////////////////////////////////////////////////

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE
#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200112L

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#include <fcntl.h>

////////////////////////////////////////////////////////////////////////////////
//                              Defines and Types                             //
////////////////////////////////////////////////////////////////////////////////

// Set default options
#define OUTPUT_FILE "filter.out"  // Default output file name
#define SEQ_COUNT   LLONG_MAX  // Max number of sequences to extract
#define ANNOT_CNT   INT_MAX    // default = ALL, -# = write first # annotation fields without the sequence, 0 = NONE,  # = write first # annotation fields with sequence
#define BYTES_LIMIT LLONG_MAX  // Max number of bytes to extract
#define PIPE_MODE   0          // 0 = NONE, 1 = HMMER, 2 = MUSCLE
#define SEARCH_MODE 0          // 0 = NONE, 1 = ENABLE 
#define VERBOSE_OPT 0          // 0 = OFF, 1 = ON
#define TRACE_OPT   0          // 0 = OFF, 1 = ON

// Set internal configurations (Do not change)
#define MAXARG_CNT  5	       // Max number of (range) sequence length options
#define FILE_LEN    128        // FILENAME_MAX, Max length for filenames
#define ERROR       -1         // Error code from failed functions
#define CFGERROR    -2         // Error code from invalid configuration

#define IMAP_LIMIT  (1LL<<28)  // Memory map chunk limit for query file, 256MB
#define STRM_BUFSIZ (1LL<<22)  // Size of output stream buffer, 4MB
#define BCAST_LIMIT (1LL<<22)  // Size for broadcasting files, 4MB
#define HITS_ID_LEN 64LL       // Max length for BLAST table query and hit IDs
#define VERBOSE(ctx) if(verbose||trace) {ctx} // Verbose mode
#define TRACE(ctx)   if(trace) {ctx}   // Trace mode (debug)  
#define MIN(a,b)     ((a < b) ? a : b)
#define MAX(a,b)     ((a > b) ? a : b)
//#define STDIN       "redirStdin.txt"
//#define STDOUT      "redirStdout.txt"
//#define STDERR      "redirStderr.txt"

// Global variable for verbose and trace modes
static int verbose;
static int trace;

// Structure for command line arguments
typedef struct st_args
{
  char	         qf[FILE_LEN];           // Query file
  char	         of[FILE_LEN];           // Output file
  char	         sf[FILE_LEN];           // Search file to extract user defined sequences
  char           btable[FILE_LEN];       // BLAST table file, used to extract hit IDs
  long long int  rseqLen[MAXARG_CNT*2];  // Range sequence length to extract
  long long int  seqLen[MAXARG_CNT];     // Sequence length to search
  long long int  seqCnt;                 // Max number of sequences to extract
  long long int  bytesLimit;             // Max number of bytes to extract
  int            seqLenBuf;              // Number of sequence length options
  int            rseqLenBuf;             // Number of range sequence length options
  int            annotCnt;               // Number of annotation fields to extract
  int            pipeMode;               // Pipeline program after extracting sequences 
  int            searchMode;             // Flag for search file sequence extraction 
} args_t;

// Structure for managing I/O and memory map
typedef struct st_iomap
{
  long long int  xCnt;        // Total number of extracted sequences
  long long int  qfsz;        // Size of query file
  FILE          *qfd;         // File descriptor of query file
  FILE          *ofd;         // File descriptor of output file
  long long int *fileOffs;    // File offsets for query file memory mappings
  char          *iMap;        // Pointer to initial mapped memory
  char          *fMap;	      // Pointer to last mapped memory
} iomap_t;

// Structure for managing queries
typedef struct st_query
{
  long long int  buflen;      // Length of intermediate buffer
  char          *buf;         // Intermediate buffer to store sequences between memory map partitions
  char          *fbuf;        // Pointer to end of buffer
  char          *iaq;         // Pointer to start of current annotation
  char          *faq;         // Pointer to end of current annotation
  char          *isq;         // Pointer to start of current sequence
  char          *fsq;         // Pointer to end of current sequence
} query_t;

// Structure for BLAST table query and hit IDs
typedef struct st_hits
{
  long long int  total;        // Total number of lines in BLAST table file
  long long int  qtotal;       // Number of distinct query IDs in BLAST table file
  long long int  htotal;       // Number of distinct hit IDs in BLAST table file
  long long int *idxList;      // Array to index hit IDs corresponding to query IDs (MUSCLE pipeline)
  FILE          *tfd;          // File descriptor of BLAST table file 
  FILE          *ofd;          // File descriptor of output file for sequences not found
  int            pipeMode;     // Pipeline program after extracting sequences 
  int            searchMode;   // Flag for search file sequnce extraction
  int           *charVect;     // Characteristic vector used to determine sequences found or not 
  char          *iMap;         // Pointer to initial mapped memory
  char          *fMap;	       // Pointer to last mapped memory
  char         **queryList;    // List of queries in BLAST table file
  char         **hitList;      // List of hit IDs in BLAST table file
} hits_t;


typedef struct st_mpi
{
  int       procCnt;	      // Total number of processes in world
  int       procRank;         // Rank index of current process
  int       nameLen;          // Length of processor name
  char      procName[MPI_MAX_PROCESSOR_NAME];     // Processor name
  MPI_Comm  MPI_MY_WORLD;     // MPI communicator object
} mpi_t;


////////////////////////////////////////////////////////////////////////////////
//                              Utility Functions                             //
////////////////////////////////////////////////////////////////////////////////

// Compute wall time of code region
double get_wtime()
{
  struct timeval tm;
  double wtm;

  if( gettimeofday(&tm, NULL) != 0 )
  {
    fprintf(stdout, "Warning: wall time function failed\n");
    return 0;
  }

  wtm = (double)tm.tv_sec + (double)tm.tv_usec * 0.000001;
  
  return wtm;
}

  
// Display help message
int displayHelp()
{
  fprintf(stdout, "\n");
  fprintf(stdout, "Description of filterfasta program\n");
  fprintf(stdout, "----------------------------------\n");
  fprintf(stdout, "NORMAL MODE:   parses sequences from a query (ungapped) FASTA file and writes the sequences to an output FASTA file\n\n");
  fprintf(stdout, "PIPELINE MODE: use a BLAST results file in tabular form to parse hit sequences from the FASTA database used and write the sequences to output FASTA files\n\n"); 
  fprintf(stdout, "\n");
  fprintf(stdout, "Help menu of filterfasta program\n");
  fprintf(stdout, "--------------------------------\n");
  fprintf(stdout, "Usage: filterfasta -q INFILE [-h] [-v] [-z] [-o OUTFILE] [-c SEQCOUNT] [-l SEQLEN | -l SEQLEN1:SEQLEN2] [-a ANNOTCOUNT] [-b BYTESLIMIT] [-t BLASTTABLE -p PIPEPROG] [-s SEARCHFILE]\n\n");
  fprintf(stdout, "-q, --query=INFILE      input query FASTA file\n");
  fprintf(stdout, "-h, --help              display this help menu\n");
  fprintf(stdout, "-v, --verbose           display processing info\n");
  fprintf(stdout, "-z, --trace             display extensive processing info for debugging\n");
  fprintf(stdout, "-o, --output=OUTFILE    output FASTA file\n");
  fprintf(stdout, "-c, --count=SEQCOUNT    number of sequences to extract from query file\n");
  fprintf(stdout, "-l, --length=SEQLEN     exact length of sequences to extract\n");
  fprintf(stdout, "-l, --length=SEQLEN1:SEQLEN2  range length of sequences to extract\n");
  fprintf(stdout, "-a, --annot=ANNOTCOUNT  number of in-order fields in annotations to extract\n");
  fprintf(stdout, "-b, --bytes=BYTESLIMIT  upper bound size for output file\n");
  fprintf(stdout, "-t, --table=BLASTTABLE  input BLAST results file in tabular form\n");
  fprintf(stdout, "-p, --pipe=PIPEMODE     pipeline mode (1 = HMMER, 2 = MUSCLE)\n");
  fprintf(stdout, "-s, --search=SEARCHFILE input annotation file to search for sequences and extract\n");
  fprintf(stdout, "\n");     
  exit(0);
  
  return 0;
}


// Parse and validate command line options
int parseCmdline(int argc, char **argv, args_t *args, mpi_t *mpi)
{
  char *token;               // Used to parse range lengths
  char suffix[2];            // Suffix for setting output file size limit 
  char loptarg[128];         // Temporary command line argument for parsing numerics
  int opt;                   // Current command line option in form of char
  int optIdx;                // Index of current command line option
  int rseqFlag;              // Count number of range lengths provided
  int ret = 0;               // Trap errors
  int found;                 // Flag to prevent repeated length options
  long long int i;           // Loop iteration variable
  long long int multiplier;  // Multiplier for setting output file size limit
  long long int optlen;      // Length of command line argument
  long long int testOpt;     // Used to validate command line options
  long long int startLen;    // Local start length for selecting sequences
  long long int endLen;      // Local end length for selecting sequences

  const struct option longOpts[] =
   {
     // These options set a value to a flag
     // Use '--{string}'
     // Example: {"debug", no_argument, &dbg, 1}
     {"verbose", no_argument,       NULL, 'v'},
     {"help",    no_argument,       NULL, 'h'}, 
     {"trace",   no_argument,       NULL, 'z'}, 

     // These options do not set a flag
     // Use '-{char}' or '--{string}'
     // Example {"debug", no_argument, NULL, 'd'}
     {"query",   required_argument, NULL, 'q'},
     {"output",  required_argument, NULL, 'o'},
     {"search",  required_argument, NULL, 's'},
     {"count",   required_argument, NULL, 'c'},
     {"length",  required_argument, NULL, 'l'},
     {"annot",   required_argument, NULL, 'a'},
     {"bytes",   required_argument, NULL, 'b'},
     {"table",   required_argument, NULL, 't'},
     {"pipe",    required_argument, NULL, 'p'},

     // The last element has to be filled with zeros
     {0, 0, 0, 0}
   };

  // Set default values to args_t structure
  strncpy(args->of, OUTPUT_FILE, FILE_LEN);
  args->seqCnt = SEQ_COUNT;
  args->seqLenBuf = 0;
  args->rseqLenBuf = 0;
  args->annotCnt = ANNOT_CNT;
  args->bytesLimit = BYTES_LIMIT;
  args->pipeMode = PIPE_MODE;
  args->searchMode = SEARCH_MODE;
  
  // Default verbose option is off
  verbose = VERBOSE_OPT;

  // Default verbose option is off
  trace = TRACE_OPT;
  
  // Iterate through all the command line arguments 
  optIdx = 0;
  while( 1 )
  {
    // Get command line option
    opt = getopt_long(argc, argv, ":q:o:s:c:l:a:b:t:p:vhz", longOpts, &optIdx);
    
    // If all command line options have been parsed, exit loop
    if( opt == -1 ) break;
    
    // Validate current option character
    switch( opt )
    {
      case 0:   // options that set a flag variable
          break;

      case 'h':	// help option
          displayHelp();
          break;
  
      case 'q':	// query file
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
    
          strncpy(args->qf, optarg, FILE_LEN);
          break;
 
      case 'o':	// output file
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
    
          strncpy(args->of, optarg, FILE_LEN);
          break;

      case 's':	// search file
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
    
          strncpy(args->sf, optarg, FILE_LEN);
          args->searchMode = 1;
          break;

      case 'c':	// max sequence count
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
    
          testOpt = strtoll(optarg, NULL, 10);
          if( testOpt < 0LL )
          {
            fprintf(stderr, "\nConfig error: invalid sequence count value = %lld (count has to be 1 or greater)\n", testOpt);
            ret = ERROR;	  
            break;
          }
          args->seqCnt = testOpt;
          break;

      case 'l':	// sequence length
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
    
          // Check if it is in range sequence length format
          strncpy(loptarg, optarg, FILE_LEN);
          token = strchr(loptarg, ':');
          if( token != NULL )
          {
            // If only ':' was supplied, then consider all lengths
            startLen = 0LL;
            endLen = SEQ_COUNT;

            // If begins with ':', then set implicit start length for range
            rseqFlag = 0;
            if( *loptarg == ':')
              rseqFlag = 1;

            // If ends with ':', then set implicit end length for range 
            token = strchr(loptarg, '\0');
            token--;
            if( *token == ':' )
              rseqFlag = rseqFlag + 2;
         
            // Parse string for start and end lengths
            if( loptarg != token )
            {   
              // If ':' at beginning and end of option, then invalid option
              if( rseqFlag > 2 )
              {
                fprintf(stderr, "\nConfig error: too many values for range format = %s\n", optarg);
                ret = ERROR;
                break;
              }
              // If ':' at beginning of option only or at end of option only
              else if( (rseqFlag == 1) || (rseqFlag == 2) )
              {
                // Search for the end of range length
                token = strtok(loptarg, ":");
                testOpt = strtoll(token, NULL, 10);
                if( testOpt < 0LL )
                {
                  fprintf(stderr, "\nConfig error: invalid range sequence length value = %lld (length has to be 0 or greater)\n", testOpt);
                  ret = ERROR;
                  break;
                }

                // Too many values, invalid option
                token = strtok(NULL, ":");
                if( token != NULL )
                {
                  fprintf(stderr, "\nConfig error: invalid format, too many range values specified = %s\n", optarg);
                  ret = ERROR;
                  break;
                }

                // If ':' at beginning only, validate range value
                if( rseqFlag == 1 )
                  endLen = testOpt;
                // If ':' at end only, validate range value
                else
                  startLen = testOpt;
              }
              // If ':' not at beginning nor end of option
              else
              {
                // Parse the start and end range lengths
                token = strtok(loptarg, ":");
                startLen = strtoll(token, NULL, 10);
                if( startLen < 0LL )
                {
                  fprintf(stderr, "\nConfig error: invalid start range sequence length value = %lld (length has to be 0 or greater)\n", startLen);
                  ret = ERROR;
                  break;
                }

                token = strtok(NULL, ":");
                endLen = strtoll(token, NULL, 10);
                if( endLen < 1LL )
                {
                  fprintf(stderr, "\nConfig error: invalid end range sequence length value = %lld (length has to be 1 or greater)\n", endLen);
                  ret = ERROR;
                  break;
                }
              
                // Too many values, invalid option
                token = strtok(NULL, ":");
                if( token != NULL )
                {
                  fprintf(stderr, "\nConfig error: invalid format, too many range values specified = %s\n", optarg);
                  ret = ERROR;
                  break;
                }
              }  
            }
 
            // If end of range is less than or equal to start value, then invalid option
            if( endLen <= startLen )
            {
              fprintf(stderr, "\nConfig error: invalid start/end range values = %s (start range cannot be greater than or equal to end range)\n", optarg);
              ret = ERROR;
              break;
            }

            // Valid range option, allow multiple range length options
            if( args->rseqLenBuf < MAXARG_CNT )
            {
              // Check if current length has already been provided
              found = 0;
              for(i = 0LL; i < args->rseqLenBuf; i++)
              {
                if( startLen == args->rseqLen[i*2] && endLen == args->rseqLen[(i*2)+1] )
                {
                  found = 1;
                  break;
                }
              }
              
              if( found == 0 )
              {
                args->rseqLen[args->rseqLenBuf*2] = startLen;
                args->rseqLen[(args->rseqLenBuf*2)+1] = endLen;
                args->rseqLenBuf++;
              }
            }
            else
            { 
              fprintf(stderr, "\nWarning: reached limit on sequence range length options allowed, ignoring length option =  %s\n", optarg);
            }
          }
          // It is in single sequence length format
          else
          {
            testOpt = strtoll(optarg, NULL, 10);
            if( testOpt < 0LL )
            {
              fprintf(stderr, "\nConfig error: invalid sequence length value = %lld (length has to be 0 or greater)\n", testOpt);
              ret = ERROR;
              break;
            }

            // Allow multiple single sequence length options
            if( args->seqLenBuf < MAXARG_CNT )
            {
              // Check if current length has already been provided
              found = 0;
              for(i = 0LL; i < args->seqLenBuf; i++)
              {
                if( testOpt == args->seqLen[i] )
                {
                  found = 1;
                  break;
                }
              }
             
              // If length is unique, add to setting 
              if ( found == 0 )
              {
                args->seqLen[args->seqLenBuf] = testOpt;
                args->seqLenBuf++;
              }
            }
            else
            {
              fprintf(stderr, "\nWarning: reached limit on sequence length options allowed, ignoring length option = %lld\n", testOpt);
            }
          }
          break;

      case 'a':	// annotation field count
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
    
          testOpt = strtoll(optarg, NULL, 10);
          if( testOpt == INT_MIN )
          {
            fprintf(stderr, "\nConfig error: invalid annotation field count = %lld (annotation has to be between %d and %d)\n", testOpt, INT_MIN+1, INT_MAX);
            ret = ERROR;
            break;
          }
          args->annotCnt = (int)testOpt;
          break;

      case 'b': // max number of bytes to extract
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
   
          // Check if a size suffix was specified
          optlen = (long long int)strlen(optarg);
          if( (isalpha((int)*(optarg+optlen-1)) != 0) && (isalpha((int)*(optarg+optlen-2)) != 0) )
          {
            strncpy(loptarg, optarg, optlen-2);
            *(loptarg+optlen-2) = '\0'; 
            strncpy(suffix, optarg+optlen-2, 2);
            
            // Set multiplier according to valid suffixes
            // KB=2^10, MB=2^20, GB=2^30
            suffix[0] = (char)toupper((int)suffix[0]);
            suffix[1] = (char)toupper((int)suffix[1]);
            if( strncmp(suffix, "KB", 2) == 0 )
              multiplier = (1LL<<10);
            else if( strncmp(suffix, "MB", 2) == 0 )
              multiplier = (1LL<<20);
            else if( strncmp(suffix, "GB", 2) == 0 )
              multiplier = (1LL<<30);
            else   // invalid suffix
            {
              fprintf(stderr, "\nConfig error: invalid suffix in byte limit\n");
              ret = ERROR;
              break;
            }
          }
          // No suffix represents bytes
          else
          {
            multiplier = 1LL;
            strncpy(loptarg, optarg, optlen);
          }

          testOpt = strtoll(loptarg, NULL, 10);
          if( testOpt < 1LL )
          {
            fprintf(stderr, "\nConfig error: invalid number of bytes limited = %lld (bytes has to be 1 or greater)\n", testOpt);
            ret = ERROR;
            break;
          }
          args->bytesLimit = testOpt * multiplier;
          break;
    
      case 'v': // verbose mode
          verbose = 1;
          break;

      case 'z': // trace mode
          trace = 1;
          break;

      case 't': // BLAST table file
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
    
          strncpy(args->btable, optarg, FILE_LEN);
          break;

      case 'p': // select pipeline program
          // If '=' at beginning of argument, ignore it
          if( *optarg == '=' ) optarg++;
    
          testOpt = strtoll(optarg, NULL, 10);
          if( testOpt < 0LL || testOpt > 2LL )
          {
            fprintf(stderr, "\nConfig error: invalid pipe program setting = %lld (0, NONE, 1 = HMMER, 2 = MUSCLE)\n", testOpt);
            ret = ERROR;
            break;
          }
          args->pipeMode = (int)testOpt;
          break;

      case '?': // unknown option
          // A getopt_long error occurred due to non-valid option or missing argument to option
          // getopt_long prints error automatically
          fprintf(stderr, "\nConfig error: unknown option (%c)\n", optopt);
          ret = ERROR;
          break;
      
      case ':': // missing option argument
          fprintf(stderr, "\nConfig error: missing option argument (%c)\n", optopt);
          ret = ERROR;
          break;

      default: // unknown return value from getopt_long()
          fprintf(stderr, "\nConfig error: unknown return value from getopt_long()\n");
          ret = ERROR;
          break;
    }
  }
 
  // Exit if error
  //if( ret != 0 ) return ret;

  // Check that pipeline mode and search mode are not both set
  if( args->pipeMode != 0 && args->searchMode != 0 )
  {
    fprintf(stderr, "\nConfig error: conflict between pipeline and search mode\n");
    ret = ERROR;
  }
 
  // Validate that input and output files do not refer to the same files 
  // Check that an input query file
  if( strlen(args->qf) == 0 )
  {
    fprintf(stderr, "\nConfig error: missing query file\n");
    ret = ERROR;
  }
  else
  {
    // Check that input query file and output file are not the same
    // Do not allow file overwriting
    if( strncmp(args->qf, args->of, FILE_LEN) == 0 )
    {
      fprintf(stderr, "\nConfig error: query and output file refer to the same file\n");
      ret = ERROR;
    }
    
    // Check if search file will be used
    if( args->searchMode != 0 )
    {
      // Check that input query file and search file are not the same
      if( strncmp(args->qf, args->sf, FILE_LEN) == 0 )
      {
        fprintf(stderr, "\nConfig error: query and search file refer to the same file\n");
        ret = ERROR;
      }
    }
  } 

  // Validation for pipeline mode 
  if( args->pipeMode != 0 )
  {
    // Check if BLAST table file was provided for extracting hit IDs 
    if( strlen(args->btable) == 0 )
    {
      fprintf(stderr, "\nConfig error: BLAST table file was not provided for pipeline\n");
      ret = ERROR;
    }
    else
    {
      // Check that BLAST table file and input query file are not the same
      if( strncmp(args->btable, args->qf, FILE_LEN) == 0 )
      {
        fprintf(stderr, "\nConfig error: BLAST table and query file refer to the same file\n");
        ret = ERROR;
      }
      // Check that BLAST table file and output file are not the same
      // Do not allow file overwriting
      if( strncmp(args->btable, args->of, FILE_LEN) == 0 )
      {
        fprintf(stderr, "\nConfig error: BLAST table and output file refer to the same file\n");
        ret = ERROR;
      }
    }
  }
  else if( strlen(args->btable) != 0 )
  {
    fprintf(stdout, "\nWarning: ignoring BLAST table file, pipeline mode is not set\n");
  }

  // Exit if error
  if( ret != 0 ) return ret;
 
  // If valid command line options, print them
  if( verbose == 1 )
  {
    fprintf(stdout, "\n--------------Configuration--------------\n");
    if( mpi->procCnt > 1 )
      fprintf(stdout, "MPI enabled (process %d of %d in %s)\n", mpi->procRank, mpi->procCnt, mpi->procName);
    fprintf(stdout, "Query file = %s\n", args->qf);
    fprintf(stdout, "Output file = %s\n", args->of);
    fprintf(stdout, "Max sequence count = %lld\n", args->seqCnt);
    fprintf(stdout, "Max bytes of output file = %lld\n", args->bytesLimit);
    if( args->seqLenBuf == 0 && args->rseqLenBuf == 0 )
      fprintf(stdout, "Sequence length = ALL\n");
    else
    {
      for(i = 0LL; i < args->seqLenBuf; i++)
        fprintf(stdout, "Sequence length [%lld] = %lld\n", i+1, args->seqLen[i]);
      for(i = 0LL; i < args->rseqLenBuf; i++)
        fprintf(stdout, "Range sequence length [%lld] = [%lld-%lld]\n", i+1, args->rseqLen[i*2], args->rseqLen[(i*2)+1]);
    }
    if( args->annotCnt == INT_MAX )
      fprintf(stdout, "Annotation field count = ALL\n");
    else if( args->annotCnt == 0 )
      fprintf(stdout, "Annotation field count = NONE\n");
    else
      fprintf(stdout, "Max annotation field count = %d %s sequences\n", abs(args->annotCnt), ((args->annotCnt < 0)?"without":"with"));
    if( args->pipeMode == 0 )
      fprintf(stdout, "BLAST pipeline program = NONE\n");
    else if( args->pipeMode == 1 )
      fprintf(stdout, "BLAST pipeline program = HMMER\n");
    else if( args->pipeMode == 2 )
      fprintf(stdout, "BLAST pipeline program = MUSCLE\n");
    if( args->pipeMode != 0 )
      fprintf(stdout, "BLAST table file = %s\n", args->btable);
    if( args->searchMode != 0 )
      fprintf(stdout, "Search file = %s\n", args->sf);

    // Print any remaining command line arguments (not options)
    if( optind < argc )
    {
      fprintf(stdout, "Ignoring non-option ARGV-elements: ");
      while( optind < argc )
        fprintf(stdout, "%s ", argv[optind++]);
      fprintf(stdout, "\n");
    }
  }
  
  return 0;
}


// Get sequence annotations
int getAnnot(iomap_t *iomap, query_t *query)
{
  char *p;  // Temporary pointer to move through annotations

  // Find start of query
  p = query->fsq;
  while( 1 )
  {
    // Reached end of memory map
    if( p == iomap->fMap || p == query->fbuf )
    {
      return ERROR;
    }
    // Found start of query
    else if( *p == '>' )
    {
      query->iaq = p;
      break;
    }

    // Move one character forward
    p++;
  }

  // Find end of sequence annotations
  p = p + 1;
  while( 1 )
  {
    // Reached end of memory map
    if( p == iomap->fMap || p == query->fbuf )
    {
      return ERROR;
    }
    // Found end of annotations
    else if( *p == '\n' )
    {
      query->faq = p;
      break;
    }

    // Move one character forward
    p++;
  }

  return 0;
}


// Get sequence data
int getSequence(long long int *seqSz, iomap_t *iomap, query_t *query)
{
  char *p;  // Temporary pointer to move through sequence data

  // Set start of sequence data
  *seqSz = 0LL;
  query->isq = query->faq + 1;

  // Find end of sequence data
  p = query->isq;
  while( 1 )
  {
    // Reached end of memory map
    if( p == iomap->fMap || p == query->fbuf )
    {
      // Set pointer to possible end of query
      query->fsq = p;    
      break;
    }
    // Found a newline, sequence continues but do not count as size
    else if( *p == '\n' )
    {
      p++;
      continue; 
    }
    // Found start of next query, set pointer to end of current query sequence
    else if( *p == '>' )
    {
      query->fsq = p - 1;
      break;
    }
    
    // Increments for each character in sequence
    (*seqSz)++;
    p++;
  }  

  // No sequence data found
  if( *seqSz == 0LL )
  {
    query->isq = query->faq;
    query->fsq = query->faq;
    fprintf(stdout, "\nError: no sequence data found\n");
    return ERROR;
  }

  return 0;
}


// Set up memory map of input query file using the specified size
int initQueryMap(long long int msz, long long int offset, iomap_t *iomap, mpi_t *mpi)
{
  // Create memory map of input query file
  offset = offset + iomap->fileOffs[mpi->procRank*3];
  msz = msz + iomap->fileOffs[mpi->procRank*3+1];
  iomap->iMap = (char *)mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fileno(iomap->qfd), offset);
  if( iomap->iMap == MAP_FAILED )
  {
    fprintf(stderr, "\n");
    perror("mmap()");
    return ERROR;
  }

  // Advise kernel on how to handle memory maps
  posix_madvise(iomap->iMap, msz, POSIX_MADV_SEQUENTIAL | POSIX_MADV_WILLNEED);

  // Lock map to memory
  if( mlock(iomap->iMap, msz) != 0 )
    fprintf(stdout, "Warning: Process %d, failed to lock map\n", mpi->procRank);

  // Update mapping pointers
  iomap->fMap = (iomap->iMap + msz) - 1;
  iomap->iMap = iomap->iMap + iomap->fileOffs[mpi->procRank*3+1];

  return 0;
}


// Open input query file
int openQueryFile(char *fn, iomap_t *iomap)
{
  struct stat stbuf;

  // Open input query file
  iomap->qfd = fopen(fn, "rb");
  if( iomap->qfd == NULL )
  {
    fprintf(stderr, "\n");
    perror("fopen()");
    return ERROR;
  }

  // Set file size
  // Check if file is empty
  fstat(fileno(iomap->qfd), &stbuf);
  if( stbuf.st_size <= 0L )
  {
    fprintf(stderr, "\nError: query file is empty\n");
    fclose(iomap->qfd);
    return ERROR;
  }
  iomap->qfsz = (long long int)stbuf.st_size;

  // Set buffering options, _IOFBF = full buffering of size STRM_BUFSIZ
  setvbuf(iomap->qfd, NULL, _IOFBF, STRM_BUFSIZ);
 
  return 0;
}


// Parse annotations
int parseAnnot(int annotCnt, long long int *annotSz, query_t *query)
{
  char *p;   // Temporary pointer to move through the annotations

  // Loop through the annotations until the requested field count is found or end of annotation is reached. Bytes to write are computed. 
  p = query->iaq;
  while( 1 )
  {
    // Reached end of annotations, need to write complete annotation
    // Use size set in getSequence()
    if( p == query->faq )
    {
      *annotSz = *annotSz - 1;
      break;
    }
    // Found an annotation field.
    // Delimited by either '|' (vertical bar) or '^A' (start of heading = 1) 
    else if( (*p == '|') || ((int)(*p) == 1) )
    {
      // Decrement fields found
      annotCnt--;

      // Check if number of requested fields have been found
      if( annotCnt == 0 )
      {
        // Compute bytes to write
        *annotSz = (long long int)(p - query->iaq); 
        break;
      }
    }
    
    // Move forward one character 
    p++;
  }

  return 0;
}


// Extract queries in current memory map
int extractQueries(args_t *args, iomap_t *iomap, query_t *query, hits_t *hits, mpi_t *mpi, long long int *bytesWritten, int *done)
{
  char *paq;                   // Pointer for comparing multiple query annotations
  int err;                     // Trap errors
  int seqSelect;               // Flag for sequences selected
  long long int hlen;          // Length of hit ID
  long long int lerr;          // Trap number of elements written by write()
  long long int annotSz;       // Size of annotations in bytes
  long long int seqSz;         // Size of sequence data in bytes
  long long int rawSeqSz;      // Size of sequence data in bytes before parsing
  long long int wCnt;          // Bytes to write
  long long int i;             // Loop iteration variable

  // Loop until end of mapped memory is reached or sequence count quota is reached
  while( 1 )
  {
    // Sequence count limit only works in serial mode
    if( mpi->procCnt == 1 )
    {
      // Normal mode: sequence cuota is met, set done flag
      if( iomap->xCnt == args->seqCnt )
      {
        *done = 1;
        break;
      }
  
      // Pipeline or search mode: sequence quota is met, set done flag
      if( hits->pipeMode != 0 || hits->searchMode != 0 )
      {
        if( iomap->xCnt == hits->htotal )
        {
          *done = 1;
          break;
        }
      }
    }
    
    // Get next sequence annotations
    err = getAnnot(iomap, query);
    if( err != 0 )
      break;
      
    // Compute annotation size
    annotSz = (long long int)(query->faq - query->iaq + 1);
  
    // Get next query sequence
    err = getSequence(&seqSz, iomap, query);
    if( err != 0 )
      break;
   
    // Compute raw sequence size
    rawSeqSz = (long long int)(query->fsq - query->isq + 1);

    // Reset sequence selected flag
    seqSelect = 0;
    
    // Perform BLAST hits table filtering
    if( hits->pipeMode != 0 || hits->searchMode != 0 )
    {
      // Need to compare all sequences in query file with table file, do not select anything yet
      for(i = 0LL; i < hits->htotal && seqSelect == 0; i++)
      {
        // Compare hit ids and first annotation ids
        // Plus 1 to skip ">" at beginning of each sequence
        hlen = (long long int)strlen(hits->hitList[i]);
        if( strncmp(hits->hitList[i], query->iaq+1, hlen) == 0 )
        {
          seqSelect = 1;
          hits->charVect[i] = 1;
          continue;
        }
        
        // Compare hit ids and remaining annotation ids
        paq = query->iaq;
        while( paq != query->faq )
        {
          // "Start of Heading" symbol delimits multiple annotations of a single query
          paq++;
          if( *paq == (char)1 )
          {
            if( strncmp(hits->hitList[i], paq+1, hlen) == 0 )
            {
              // If annotations require parsing, begin at matched annotation
              if( args->annotCnt != 0 )
              {
                *paq = '>';
                query->iaq = paq;
              }
              seqSelect = 1;
              hits->charVect[i] = 1;
              break;
            } 
          } 
        }
      }
    }
    // Perform normal filtering
    else
    {
      // If single and range sequence lengths were not provided, then extract all sequences
      if( (args->seqLenBuf == 0) && (args->rseqLenBuf == 0) )
      {
        seqSelect = 1;
      }
      // Either single or range sequence lengths were provided
      else
      {
        // If single sequence lengths were provided
        if( args->seqLenBuf > 0 )
        {
          // Check if size of sequence is the desired
          for(i = 0LL; (i < args->seqLenBuf) && (args->seqLen[i] == seqSz); i++)
          {
            seqSelect = 1;
            break;
          }
        }
      
        // If range sequence lengths were provided
        if( args->rseqLenBuf > 0 )
        {
          // Check if size of sequence is the desired
          for(i = 0LL; (i < args->rseqLenBuf) && (args->rseqLen[i*2] <= seqSz) && (args->rseqLen[(i*2)+1] >= seqSz); i++)
          {
            seqSelect = 1;
            break;
          }
        }
      }
    } 

    // If the current query was selected, prepare it for output
    if( seqSelect == 1 )
    {
      // (default) Do not parse annotations, write all including sequences
      if( args->annotCnt == INT_MAX || args->annotCnt == INT_MIN+1 )
      {
        // Check if next entire query (raw annotations and sequence) fits in output file based on size limit option
        if( args->annotCnt == INT_MAX )
          wCnt = annotSz + rawSeqSz;
        // Check if next entire raw annotations fits in output file based on size limit option
        else
          wCnt = annotSz;
        
        // Reached limit on number of bytes, we are done
        if( (wCnt + *bytesWritten) > args->bytesLimit )
        {
          *done = 1;
          break;
        }

        // Write complete sequence to file
        // Write annotation
        lerr = fwrite(query->iaq, sizeof(char), wCnt, iomap->ofd);
        *bytesWritten = *bytesWritten + lerr;
      }
      // Parse annotations
      else if( args->annotCnt != 0 )
      {
        // Find how many bytes to use from annotations and write them
        parseAnnot(abs(args->annotCnt), &annotSz, query);

        if( args->annotCnt > 0 )
        {
          // Check if next entire query (raw annotations and sequence) fits in output file based on size limit option
          wCnt = annotSz + rawSeqSz + 1; 
        
          // Reached limit on number of bytes, we are done
          if( (wCnt + *bytesWritten) > args->bytesLimit )
          {
            *done = 1;
            break;
          }

          // Write annotation
          lerr = fwrite(query->iaq, sizeof(char), annotSz, iomap->ofd);
          *bytesWritten = *bytesWritten + lerr;
        
          lerr = fwrite("\n", sizeof(char), 1, iomap->ofd);
          *bytesWritten = *bytesWritten + lerr;

          // Write sequence data
          lerr = fwrite(query->isq, sizeof(char), rawSeqSz, iomap->ofd);
          *bytesWritten = *bytesWritten + lerr;
        }
        else
        {
          // Check if next entire raw annotations fits in output file based on size limit option
          wCnt = annotSz; 

          // Reached limit on number of bytes, we are done
          if( (wCnt + *bytesWritten) > args->bytesLimit )
          {
            *done = 1;
             break;
          }

          // Write annotation without ">" symbol
          lerr = fwrite(query->iaq+1, sizeof(char), annotSz-1, iomap->ofd);
          *bytesWritten = *bytesWritten + lerr;
          
          lerr = fwrite("\n", sizeof(char), 1, iomap->ofd);
          *bytesWritten = *bytesWritten + lerr;
        }
      }
      // Do not write annotations
      else
      {
        // Check if next entire query (raw annotations and sequence) fits in output file based on size limit option
        // Reached limit on number of bytes, we are done
        wCnt = rawSeqSz;
        if( (wCnt + *bytesWritten) > args->bytesLimit )
        {
          *done = 1;
          break;
        }

        // Write sequence data
        lerr = fwrite(query->isq, sizeof(char), wCnt, iomap->ofd);
        *bytesWritten = *bytesWritten + lerr;
      } 
   
      // Count sequences written to output file    
      iomap->xCnt++;
    }
  }

  return 0;
}


// Adjust memory map to data in query file based on a requested size
// Copies first section of query to a temporary buffer in case it lies between partitions
int adjustMapBegin(long long int *offset, iomap_t *iomap, query_t *query)
{
  char *c;                // Character read
  long long int buflen;   // New length of temporary buffer
  long long int i;        // Iteration variable

  // Read current memory map in order
  i = 0LL;
  c = iomap->iMap;
  while( c != iomap->fMap )
  {
    // End-of-file, no adjustment
    if( (int)(*c) == EOF )
    {
      fprintf(stdout, "\nError: end-of-file detected in memory map in adjustMapBegin()\n");
      return ERROR;
    }
    // Found the beginning of a sequence
    else if ( *c == '>' )
    {
      // Set map to begin with this sequence
      *offset = i;
      
      // Query found and data pertaining to previous query in temporary buffer 
      if( *offset > 0LL )
      {
        // Reallocate buffer to fit new data lying between memory maps
        buflen = query->buflen + *offset;
        query->buf = (char *)realloc(query->buf, buflen);
        if( query->buf == NULL )
        {
          fprintf(stdout, "\nError: failed to reallocate buffer for inter-mapped queries in adjustMapBegin()\n");
          return ERROR;
        }
 
        // Set pointer to end of buffer
        query->fbuf = query->buf + buflen - 1;

        // Copy query
        memcpy(query->buf + query->buflen, iomap->iMap, *offset);
        query->buflen = buflen;

        // Adjust initial pointer of memory map
        iomap->iMap = iomap->iMap + *offset;
      }

      return 0;
    }

    // Read next character
    c++;
    i++;
  }

  // Print error since we should have reached at least 1 query in memory map
  // If a single query is larger than one map, not correct behavior
  fprintf(stdout, "\nError: end of memory map reached in adjustMapBegin(), no query found\n");

  return ERROR;
}

// Adjust memory map to data in query file based on a requested size
// Copies last query to a temporary buffer in case it lies between partitions
int adjustMapEnd(iomap_t *iomap, query_t *query)
{
  char *c;          // Character read
  long long int i;  // Iteration variable
  
  // Read current memory map in reverse order starting at the requested size
  i = 0LL;
  c = iomap->fMap;
  while( c != iomap->iMap )
  {
    // End-of-file, no adjustment
    if( (int)(*c) == EOF )
    {
      fprintf(stdout, "\nError: end-of-file detected in memory map in adjustMapEnd()\n");
      return ERROR;
    }
    // Found the beginning of a sequence, copy last query to temp buffer
    else if ( *c == '>' )
    {
      // Adjust end pointer of memory map  
      iomap->fMap = iomap->fMap - (i + 1);
 
      // Allocate buffer
      query->buflen = i + 1;
      query->buf = (char *)malloc(sizeof(char) * query->buflen);
      
      // Set pointer to end of buffer
      query->fbuf = query->buf + query->buflen - 1;
      
      // Copy last query because maybe it lies between partitions
      memcpy(query->buf, iomap->fMap + 1, query->buflen);
    
      return 0;  
    }
    
    // Read in reverse order
    c--;
    i++;
  }
  
  // Print error since we should have reached at least 1 query in memory map
  // If a single query is larger than one map, not correct behavior
  fprintf(stdout, "\nError: beginning of memory map reached in adjustMapEnd(), no query found\n");

  return ERROR;
}

// Combines output files into a single file
int combineOutputFiles(args_t *args, iomap_t *iomap, mpi_t *mpi, long long int bytesWritten)
{
  int i;
  long long int *dataLen;
  long long int bytesRead;
  long long int bytesWrite;
  char *dataBuf;
  char *outbuf;
  FILE *ofd = NULL;
  MPI_Status status;
  long long int out_off;
  long long int curr_off;
  long long int curr_sz;
  long long int next_sz;
  int fileFlag;
  long long int totalBytesWritten;

  // Single process, do nothing
  if( mpi->procCnt == 1 )
    return 0;

  // Get number of bytes written by all processes 
  dataLen = (long long int *)malloc(sizeof(long long int) * mpi->procCnt);
  MPI_Gather(&bytesWritten, 1, MPI_LONG_LONG_INT, dataLen, 1, MPI_LONG_LONG_INT, 0, mpi->MPI_MY_WORLD);

  // Send all data to master for writing all results in a single file
  fileFlag = 0;
  totalBytesWritten = 0;
  out_off = 0;
  if( mpi->procRank == 0 )
  {
    // Open output file
    ofd = fopen(args->of, "wb");
    if( ofd == NULL )
    {
      fprintf(stderr, "\n");
      perror("fopen()");
      fileFlag = ERROR;
    }
    else
    {
      // Set buffering options, _IOFBF = full buffering of size STRM_BUFSIZ
      setvbuf(ofd, NULL, _IOFBF, STRM_BUFSIZ);

      // Check that some output was generated
      for(i = 0; i < mpi->procCnt; i++)
        totalBytesWritten = totalBytesWritten + dataLen[i];
      
      if( totalBytesWritten == 0 )
      {
        fclose(ofd);
        fileFlag = ERROR;
      }
      else
        // Set size and hint kernel about output file
        ftruncate(fileno(ofd), totalBytesWritten);
    }
  }
 
  // Check that master was able to create output file
  MPI_Bcast(&fileFlag, 1, MPI_INT, 0, mpi->MPI_MY_WORLD);
  if( fileFlag != 0 )
  {
    fprintf(stdout, "Error: failed to create combined output file\n");
    free(dataLen);
    return ERROR;
  }
 
  // Set file position at beginning 
  rewind(iomap->ofd);
  
  // Copy masters data
  if( mpi->procRank == 0 )
  { 
    if( dataLen[0] > 0 )
    {
      // Prepare file for copying
      posix_fadvise(fileno(iomap->ofd), 0, MIN(BCAST_LIMIT, dataLen[0]), POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
      posix_fadvise(fileno(ofd), 0, MIN(BCAST_LIMIT, totalBytesWritten), POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
   
      // Allocate a buffer for passing data
      outbuf = (char *)malloc(sizeof(char) * MIN(BCAST_LIMIT, dataLen[0]));
 
      curr_off = 0;
      next_sz = MIN(BCAST_LIMIT, dataLen[0]); 
      while (curr_off < dataLen[0])
      {
        curr_sz = next_sz;
         
        // Read the current chunk
        bytesRead = fread(outbuf, sizeof(char), curr_sz, iomap->ofd);
        if( bytesRead != curr_sz )
          fprintf(stdout, "Master did not read chunk size correctly when combining outputs, %lld\n", bytesRead);
 
        // Page-in next chunk
        curr_off = curr_off + bytesRead;
        next_sz = MIN(BCAST_LIMIT, dataLen[0] - curr_off);
        posix_fadvise(fileno(iomap->ofd), curr_off, next_sz, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
 
        // Write the current chunk 
        bytesWrite = fwrite(outbuf, sizeof(char), curr_sz, ofd);
        if( bytesWrite != curr_sz )
          fprintf(stdout, "Process did not write chunk size correctly\n");
      
        // Page-in next chunk
        out_off = curr_off;
        posix_fadvise(fileno(ofd), out_off, next_sz, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
      }

      free(outbuf);
    }

    // Copy other processes data
    for( i = 1; i < mpi->procCnt; i++)
    {
      if( dataLen[i] == 0 )
        continue;
   
      // Allocate a buffer for passing data
      outbuf = (char *)malloc(sizeof(char) * MIN(BCAST_LIMIT, dataLen[i]));
      
      // Receive data and write to file in chunks 
      curr_off = 0;
      next_sz = MIN(BCAST_LIMIT, dataLen[i]); 
      while( curr_off < dataLen[i] )
      {
        // Received chunk
        curr_sz = next_sz;
        MPI_Recv(outbuf, curr_sz, MPI_CHAR, i, 0, mpi->MPI_MY_WORLD, &status);

        // Write chunk
        bytesWrite = fwrite(outbuf, sizeof(char), curr_sz, ofd);
        if( bytesWrite != curr_sz )
          fprintf(stderr, "Error: bytes written do not match in fwrite(), partition query file\n");
        
        // Page-in next chunk
        curr_off = curr_off + bytesWrite;
        next_sz = MIN(BCAST_LIMIT, dataLen[i] - curr_off);
        out_off = out_off + curr_off;
        posix_fadvise(fileno(ofd), out_off, next_sz, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
      }

      free(outbuf);
    }

    // Free resources
    fflush(ofd);
    fclose(ofd);
  }
  else
  {
    if( bytesWritten > 0 )
    {
      // Prepare file for copying
      posix_fadvise(fileno(iomap->ofd), 0, MIN(BCAST_LIMIT, bytesWritten), POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
      
      // Allocate a buffer for passing data
      dataBuf = (char *)malloc(sizeof(char) * MIN(BCAST_LIMIT, bytesWritten));
      
      // Send file data in chunks 
      curr_off = 0;
      next_sz = MIN(BCAST_LIMIT, bytesWritten); 
      while( curr_off < bytesWritten )
      {
        // Read chunk
        curr_sz = next_sz;
        bytesRead = fread(dataBuf, sizeof(char), curr_sz, iomap->ofd);
        if( bytesRead != curr_sz )
          fprintf(stderr, "Error: bytes read do not match in fread(), partition query file\n");
       
        // Page-in next chunk 
        curr_off = curr_off + bytesRead;
        next_sz = MIN(BCAST_LIMIT, bytesWritten - curr_off);
        posix_fadvise(fileno(iomap->ofd), curr_off, next_sz, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
        
        // Send chunk
        MPI_Send(dataBuf, curr_sz, MPI_CHAR, 0, 0, mpi->MPI_MY_WORLD);
      }

      free(dataBuf);
    }
  }

  // Free bytes written array
  free(dataLen);

  return 0;
}


// Write to output file hit IDs from BLAST table file not found in query file
int writeHitsNotFound(char *of, hits_t *hits, mpi_t *mpi)
{
  char outfile[FILE_LEN];
  int *allCharVect;
  int fileFlag;
  int i;
  int bytesWrite;
  int fsize;
  struct stat stbuf;

  // Flag to signal file errors
  fileFlag = 0;

  // Master writes to file
  if( mpi->procRank == 0 )
  {
    // Create output filename
    snprintf(outfile, FILE_LEN, "%s.notFound", of);

    // Open output file
    hits->ofd = fopen(outfile, "wb");
    if( hits->ofd == NULL )
    {
      fprintf(stderr, "\n");
      perror("fopen()");
      fileFlag = ERROR;
    }
    else
      // Set buffering options, _IOFBF = full buffering of size STRM_BUFSIZ
      setvbuf(hits->ofd, NULL, _IOFBF, STRM_BUFSIZ);
  }

  // Check if file can be written
  MPI_Bcast(&fileFlag, 1, MPI_INT, 0, mpi->MPI_MY_WORLD);
  if( fileFlag != 0 )
    return ERROR;
 
  allCharVect = (int *)malloc(sizeof(int) * hits->htotal);

  // Sum all processes characteristic vector
  MPI_Reduce(hits->charVect, allCharVect, hits->htotal, MPI_INT, MPI_SUM, 0, mpi->MPI_MY_WORLD);

  // Determine which hit IDs were not found using the characteristic vector
  if( mpi->procRank == 0 )
  {
    for( i = 0; i < hits->htotal; i++ )
    {
      // If characteristic vector is zero, hit ID was not found
      if( allCharVect[i] == 0 )
      {
        bytesWrite = fwrite(hits->hitList[i], sizeof(char), strlen(hits->hitList[i]), hits->ofd);
        fputc('\n', hits->ofd);
      }
    }

    // Flush output file
    fflush(hits->ofd);
    
    // Close output file
    fstat(fileno(hits->ofd), &stbuf);
    fsize = stbuf.st_size;
    fclose(hits->ofd);

    // Remove output file if empty
    if( fsize <= 0L )
    {
      fprintf(stdout, "\nWarning: removing empty output file of hit IDs\n");
      remove(outfile);
    }
  }
 
  // Free resources 
  free(allCharVect);

  return 0;
}


// Partition query file and memory map into chunks for processing
int partQueryFile(args_t *args, iomap_t *iomap, hits_t *hits, mpi_t *mpi)
{
  char outfile[FILE_LEN];         // Output file
  int err;                        // Trap errors
  int done;                       // Flag to signal when sequence count quota has been met
  long int fsize;                 // Size of output file
  long long int mrem;             // Remaining maps to process
  long long int offset;           // Memory map offset
  long long int nmap;             // Current number of memory map
  long long int nmaps;            // Number of memory maps needed for file iteration 
  long long int msz;              // Current memory map size
  long long int psz;              // System's page size
  long long int shift;            // Bytes to shift initial map pointer
  long long int mpishift;         // Bytes to shift initial map pointer for MPI programs
  long long int xcnt;             // Count sequences extracted in current partition
  long long int bytesWritten;     // Count number of bytes written to output file
  struct stat stbuf;
  query_t query;                  // Query extraction control struct

  // Check that chunk limits of input memory map respect system's page size
  // If chunk size is less than page size, set chunks to 1024 page sizes
  // If chunk size if not a multiple of page size, set chunks to 1024 page sizes
  // Else use default chunks value
  psz = (long long int)sysconf(_SC_PAGESIZE);
  msz = IMAP_LIMIT;
  if( (msz < psz) || (msz % psz != 0) )
    msz = psz * 1024LL;	// 4MB

  // Create output filename
  if( mpi->procCnt > 1 )
    snprintf(outfile, FILE_LEN, "%s%d", args->of, mpi->procRank);
  else
    strncpy(outfile, args->of, FILE_LEN);

  // Open output file
  iomap->ofd = fopen(outfile, "w+b");
  if( iomap->ofd == NULL )
  {
    fprintf(stderr, "\n");
    perror("fopen()");
    return ERROR;
  }

  // Set buffering options, _IOFBF = full buffering of size STRM_BUFSIZ
  setvbuf(iomap->ofd, NULL, _IOFBF, STRM_BUFSIZ);
  
  VERBOSE(fprintf(stdout, "\n----------------Filtering----------------\n");)
 
  // Estimate iterations needed to process complete file in chunks
  // Later this value may be modified to fit query file appropiately
  nmaps = (long long int)ceil((double)iomap->qfsz / msz);
 
  // Process file in memory map chunks
  err = 0;
  done = 0;
  bytesWritten = 0;
  xcnt = 0LL;
  shift = 0LL;
  mpishift = iomap->fileOffs[mpi->procRank*3+1];
  memset(&query, 0, sizeof(query_t));
  for(nmap = 0; nmap < nmaps && !done; nmap++)
  {
    // Compute remaining memory maps to process
    mrem = nmaps - (nmap + 1);

    // Compute memory map offset
    offset = nmap * msz;

    // Compute memory map size for last chunk
    // Set size to remaining bytes in file
    if( mrem == 0 )
       msz = iomap->qfsz - offset;

    // Advise to kernel access pattern for file
    posix_fadvise(fileno(iomap->qfd), offset, msz, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
    
    // Debug statement
    VERBOSE(fprintf(stdout, "Processing partition %lld of %lld (%lld bytes)\n", nmap+1, nmaps, msz);)
    
    // Create memory map, if needed
    err = initQueryMap(msz, offset, iomap, mpi);
    if( err != 0 )
    {
      fprintf(stderr, "Error: failed initQueryMap()\n");
      break;
    }
    
    // Modify size of memory map to end at the before the beginning of sequence
    // Provides a way to handle sequences that are between memory map chunks
    if( nmap > 0LL )
    {
      // Adjust at beginning for this map
      err = adjustMapBegin(&shift, iomap, &query);
      if( err != 0 )
      {
        fprintf(stdout, "Error: adjustMapBegin()\n");
        munmap(iomap->iMap - shift - mpishift, msz);
        free(query.buf);
        break;
      }

      // Initialize query struct pointers
      query.iaq = query.buf;
      query.faq = query.buf;
      query.isq = query.buf;
      query.fsq = query.buf;

      // Extract queries
      err = extractQueries(args, iomap, &query, hits, mpi, &bytesWritten, &done);
      if( err != 0 )
      {
        fprintf(stderr, "\nError: failed extractQueries()\n");
        munmap(iomap->iMap - shift - mpishift, msz);
        free(query.buf);
        break;
      }

      // Reset buffer
      free(query.buf);
      query.buflen = 0;
      query.fbuf = query.buf;
    }
  
    // Initialize query struct pointers 
    query.iaq = iomap->iMap;
    query.faq = iomap->iMap;
    query.isq = iomap->iMap;
    query.fsq = iomap->iMap;
 
    // This is performed before the first call to adjustMapBegin()
    // Not last partition
    if( mrem > 0LL )
    {
      err = adjustMapEnd(iomap, &query);
      if( err != 0 )
      {
        fprintf(stdout, "Error: adjustMapEnd()\n");
        munmap(iomap->iMap - shift - mpishift, msz);
        free(query.buf);
        break;
      }
    }

    // Extract sequences from current memory map
    err = extractQueries(args, iomap, &query, hits, mpi, &bytesWritten, &done);
    if( err != 0 )
    {
      fprintf(stderr, "\nError: failed extractQueries()\n");
      munmap(iomap->iMap - shift - mpishift, msz);
      free(query.buf);
      break;
    }

    // Clear memory map
    munmap(iomap->iMap - shift - mpishift, msz);
    
    // Compute queries extracted in current partition
    xcnt = iomap->xCnt - xcnt;

    // Debug statement
    VERBOSE(fprintf(stdout, "Subtotal sequences extracted = %lld\n", xcnt);)
    xcnt = iomap->xCnt;
  }
 
  // Flush stream buffers to output file 
  fflush(iomap->ofd);

  // Check if an error occurred
  if( err != 0 )
    fprintf(stdout, "An error occurred while processing partitions\n");

  // Print statistics
  VERBOSE(fprintf(stdout, "Total sequences extracted = %lld\n", iomap->xCnt);)

  // Write to file hit IDs not found in query file
  if( hits->pipeMode != 0 || hits->searchMode != 0 )
  {
    err = writeHitsNotFound(args->of, hits, mpi);
    if( err != 0 )
      fprintf(stdout, "Error: failed to write hit IDs not found\n");
  }

#ifdef BCAST_OUTFILES 
  // Send all data sizes (bytes written) to master for writing all results in a single file
  err = combineOutputFiles(args, iomap, mpi, bytesWritten);
  if( err != 0 )
    fprintf(stdout, "Error: failed to combine output files\n");
#endif

  // Close output file
  fstat(fileno(iomap->ofd), &stbuf);
  fsize = stbuf.st_size;
  fclose(iomap->ofd);

  // Remove output file if empty
  if( fsize <= 0L )
  {
    fprintf(stdout, "\nWarning: removing empty output file\n");
    remove(outfile);
  }
  
  VERBOSE(fprintf(stdout, "\n");)

  return err;
}


// Parse BLAST query and hit IDs from current line
int parseBlastTableIDs(hits_t *hits, char *line)
{
  char *currQueryId;     // Current query ID
  char *currHitId;       // Current hit ID
  int   hexists;         // Flag to prevent duplicate hit ID for HMMER
  long long int i;       // Iteration variable
  long long int qlen;    // Length of current query ID
  long long int hlen;    // Length of current hit ID
  int qNull;
  int hNull;

  // Parse query ID from current line  
  currQueryId = strtok(line, " \t");
  if( currQueryId == NULL )
  {
    fprintf(stdout, "\nError: could not find query ID\n");
    return ERROR;
  }

  // Parse database hit ID from current line
  currHitId = strtok(NULL, " \t");
  if( currHitId == NULL )
  {
    fprintf(stdout, "\nError: could not find hit ID\n");
    return ERROR;
  }

  // Check that it fits into array
  // Plus 1 to account for null-terminating character added by strtok()
  qNull = 0;
  qlen = (long long int)strlen(currQueryId) + 1;
  if( qlen > HITS_ID_LEN )
  {
    fprintf(stdout, "\nError: current query ID is too large, size = %lld\n", qlen);
    qlen = HITS_ID_LEN - 1;
    qNull = 1;
  }

  // Check that it fits into array
  // Plus 1 to account for null-terminating character added by strtok()
  hNull = 0;
  hlen = (long long int)strlen(currHitId) + 1;
  if( hlen > HITS_ID_LEN )
  {
    fprintf(stdout, "\nWarning: current hit ID is too large, size = %lld\n", hlen);
    hlen = HITS_ID_LEN - 1;
    hNull = 1;
  }

  // Copy first query ID to list or copy query ID to list if not already in list
  if( (hits->qtotal == 0LL) || (strncmp(currQueryId, hits->queryList[hits->qtotal-1], qlen) != 0) )
  {
    strncpy(hits->queryList[hits->qtotal], currQueryId, qlen);
    if( qNull == 1 )
      hits->queryList[hits->qtotal][qlen] = '\0';
    hits->qtotal++;
  }
  
  // HMMER pipe program
  if( hits->pipeMode == 1 )
  { 
    // Copy hit ID to list if not query ID and does not exist in hit list already
    if( strncmp(currQueryId, currHitId, MIN(qlen,hlen)) != 0 )
    {
      // Do not add duplicate hit IDs, check against all in hit list
      hexists = 0;
      for(i = 0LL; i < hits->htotal; i++)
      {
        if( strncmp(currHitId, hits->hitList[i], hlen) == 0 )
        {
          hexists = 1;
          break;
        }
      }    

      // Copy hit ID to list if not query ID and does not exist in hit list already
      if( hexists == 0 )
      {
        strncpy(hits->hitList[hits->htotal], currHitId, hlen);
        if( hNull == 1 )
          hits->hitList[hits->htotal][hlen] = '\0';
        hits->htotal++;
      }
    }
  }
  else if( hits->pipeMode == 2 )
  {
    fprintf(stdout, "\nWarning: MUSCLE pipeline is still under development\n");
    return ERROR;
  }

  return 0;
}


// Free hits structure memory
int freeHitsMemory(hits_t *hits)
{
  long long int i;  // Iteration variable
    
  if( hits->pipeMode != 0 )
  {
    for(i = 0LL; i < hits->total; i++)
    {
      free(hits->queryList[i]);
      free(hits->hitList[i]);
    }
    free(hits->queryList);
    free(hits->hitList);
    free(hits->idxList);
    free(hits->charVect);
  }
  
  if( hits->searchMode != 0 )
  {
    for(i = 0LL; i < hits->total; i++)
      free(hits->hitList[i]);
    free(hits->hitList);
    free(hits->charVect);
  }
 
  return 0;
}


// Load IDs from search file for sequence extraction
int loadSearchIDs(char *fn, hits_t *hits)
{
  long int fsize;        // Size of file
  long long int i;       // Iteration variable
  long long int nch;     // Number of characters in current line
  long long int longest; // Longest line in BLAST file
  char *line;            // Current processing line
  struct stat stbuf;
  long long int len;
  int exists;
  long long int j;
  char *pch;
  int done;
 
  // Check if a search file was provided
  if( hits->searchMode == 0 )
    return 0;

  // Open search file
  hits->tfd = fopen(fn, "rb");
  if( hits->tfd == NULL )
  {
    fprintf(stderr, "\n");
    perror("fopen()");
    return ERROR;
  }

  // Check that file is not empty
  fstat(fileno(hits->tfd), &stbuf);
  fsize = stbuf.st_size;
  if( fsize <= 0L )
  {
    fprintf(stderr, "\nError: BLAST table file is empty\n");
    fclose(hits->tfd);
    return ERROR;
  }

  // Memory map file
  hits->iMap = (char *)mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fileno(hits->tfd), 0);
  if( hits->iMap == MAP_FAILED )
  {
    fprintf(stderr, "\nError: could not map table file\n");
    fclose(hits->tfd);
    return ERROR;
  }

  // Advise kernel on how to handle memory maps
  posix_madvise(hits->iMap, fsize, POSIX_MADV_SEQUENTIAL | POSIX_MADV_WILLNEED);

  // Update mapping pointers
  hits->fMap = (hits->iMap + fsize) - 1;
  
  // Close BLAST table file
  fclose(hits->tfd);

  // Compute number of lines and longest line in file
  hits->total = 0LL;
  nch = 0LL;      
  longest = 0LL;
  pch = hits->iMap;
  while( 1 )
  {
    nch++;
    if( *pch == '\n' || pch == hits->fMap )
    {  
      hits->total++;
      if( nch > longest )
        longest = nch;
      nch = 0LL;
    
      if( pch == hits->fMap )
        break;
    }
    pch++;
  }
  longest++;

  // Allocate array for search IDs
  hits->hitList = (char **)malloc(sizeof(char *) * hits->total);
  for(i = 0LL; i < hits->total; i++)
    hits->hitList[i] = (char *)malloc(sizeof(char) * HITS_ID_LEN);

  // Allocate buffer for reading lines
  line = (char *)malloc(sizeof(char) * longest);
 
  // Read file line by line and load search IDs
  hits->htotal = 0;
  pch = hits->iMap;
  done = 0;
  while( done == 0 )
  {
    // Read current line
    len = 0;
    while( 1 )
    {
      len++;
      if( *pch == '\n' || pch == hits->fMap )
      {  
        memcpy(line, pch-len+1, len);
        line[len] = '\0';
      
        if( pch == hits->fMap )
          done = 1;
        else 
          pch++;
        break;
      }
      pch++;
    }

    // Copy search ID and replace newline with null-terminating character
    if( len <= 1 )
      continue;
    else if( (len + 1) > HITS_ID_LEN )
      len = HITS_ID_LEN;
    
    // Ignore newline
    len--;  
    
    // Do not add duplicate hit IDs, check against all in hit list
    exists = 0;
    for(j = 0LL; j < hits->htotal; j++)
    {
      if( strncmp(line, hits->hitList[j], len) == 0 )
      {
        exists = 1;
        break;
      }
    }    

    // Copy search ID to list if not exists in list already
    if( exists == 0 )
    {
      strncpy(hits->hitList[hits->htotal], line, len);
      hits->hitList[hits->htotal][len] = '\0';
      hits->htotal++;
    }
  }

  // Free line buffer
  free(line);

  // Read current line
  munmap(hits->iMap, fsize);

  // Allocate characteristic vector
  hits->charVect = (int *)calloc(hits->htotal, sizeof(int));
  
  return 0;
}


// Load query and hit IDs from BLAST table file
int loadBlastTable(char *fn, hits_t *hits)
{
  char *line;            // Current processing line
  int err;               // Trap errors
  int done;
  char *pch;
  int i;                 // Iteration variable
  long int fsize;        // Size of file
  long long int nch;     // Number of characters in current line
  long long int longest; // Longest line in BLAST file
  struct stat stbuf;

  // Check if pipeline mode
  if( hits->pipeMode == 0 )
    return 0;

  // Open BLAST table file
  hits->tfd = fopen(fn, "rb");
  if( hits->tfd == NULL )
  {
    fprintf(stderr, "\n");
    perror("fopen()");
    return ERROR;
  }

  // Check that file is not empty
  fstat(fileno(hits->tfd), &stbuf);
  fsize = stbuf.st_size;
  if( fsize <= 0L )
  {
    fprintf(stderr, "\nError: BLAST table file is empty\n");
    fclose(hits->tfd);
    return ERROR;
  }

  // Memory map file
  hits->iMap = (char *)mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fileno(hits->tfd), 0);
  if( hits->iMap == MAP_FAILED )
  {
    fprintf(stderr, "\nError: could not map table file\n");
    fclose(hits->tfd);
    return ERROR;
  }

  // Advise kernel on how to handle memory maps
  posix_madvise(hits->iMap, fsize, POSIX_MADV_SEQUENTIAL | POSIX_MADV_WILLNEED);

  // Update mapping pointers
  hits->fMap = (hits->iMap + fsize) - 1;
  
  // Close BLAST table file
  fclose(hits->tfd);

  // Compute number of lines and longest line in file
  hits->total = 0LL;
  nch = 0LL;      
  longest = 0LL;
  pch = hits->iMap;
  while( 1 )
  {
    nch++;
    if( *pch == '\n' || pch == hits->fMap )
    {  
      hits->total++;
      if( nch > longest )
        longest = nch;
      nch = 0LL;
    
      if( pch == hits->fMap )
        break;
    }
    pch++;
  }
  longest++;

  // Allocate array for BLAST query IDs
  hits->queryList = (char **)malloc(sizeof(char *) * hits->total);
  for(i = 0LL; i < hits->total; i++)
    hits->queryList[i] = (char *)malloc(sizeof(char) * HITS_ID_LEN);

  // Allocate array for BLAST hit IDs
  hits->hitList = (char **)malloc(sizeof(char *) * hits->total);
  for(i = 0LL; i < hits->total; i++)
    hits->hitList[i] = (char *)malloc(sizeof(char) * HITS_ID_LEN);

  // Allocate array for hit/query index array
  hits->idxList = (long long int *)malloc(sizeof(long long int) * hits->total);

  // Allocate buffer for reading lines
  line = (char *)malloc(sizeof(char) * longest);
  
  // Read file line by line and parse BLAST query and hit IDs
  hits->qtotal = 0;
  hits->htotal = 0;
  pch = hits->iMap;
  done = 0;
  while( done == 0 )
  {
    nch = 0;
    while( 1 )
    {
      nch++;
      if( *pch == '\n' || pch == hits->fMap )
      {  
        memcpy(line, pch-nch+1, nch);
        line[nch] = '\0';
      
        if( pch == hits->fMap )
          done = 1;
        else 
          pch++;
        break;
      }
      pch++;
    }

    // Parse BLAST query and hit IDs
    err = parseBlastTableIDs(hits, line);
    if( err != 0 )
    {
      fprintf(stdout, "Error: failed parsing BLAST query and hit IDs\n");
      free(line);
      freeHitsMemory(hits);
      munmap(hits->iMap, fsize);
      return ERROR;
    }
  }
  
  // Free line buffer
  free(line);

  // Read current line
  munmap(hits->iMap, fsize);

  // Allocate characteristic vector
  hits->charVect = (int *)calloc(hits->htotal, sizeof(int));

  return 0;
}


// Function that takes a file descriptor, its data size, and partition count to compute offsets for balanced partitions.
// Partitions are guaranteed to be independent based on a separator symbol supplied by the user.
// Function returns the partition count considered and an array where every 3 contigous elements correspond to the offset values for a particular partition considered as,
// [i]=file_offset from beginning of file, [i+1]=map offset from file offset, [i+2]=total map size
// Element 1 = page-based offset from initial file position
// Element 2 = offset from element 1 where the actual independent data begins
// Element 3 = size of independent data for this partition
// For programs that mmap before, use "element 1 + element 2" as your initial address for processing. For programs that will mmap partitions after, need to respect system's page size, mmap from "element 1" and use "element 1 + element 2" as your initial address for processing.
int computePartitionOffsets(long long int **offs, int *parts, FILE *fd, long int sz, char sym)
{
  int err;

  // Invalid inputs
  if( *parts < 1 || sz < 1 )
  {
    *offs = NULL;
    fprintf(stdout, "Invalid inputs for computing partition offsets\n");
    return ERROR;
  }
 
  // Single partition 
  *offs = malloc((*parts * 3) * sizeof offs); 
  
    char *buffer;
    int i;
    int foundSeqBeg;
    int lparts;
    long long int readOffs;
    long long int partSz; 
    long long int c;
    long long int j;
    long long int offset;
    long long int seqChunks;
    long int multiplier;
    long long int chunkOffs;
    long long int prevOffs;
    long long int bytesRead;

    lparts = *parts;
    while( 1 )
    {
      // Attempt to compute offsets considering data size and partition count 
      //err = setOffsQueryFile(*offs, fd, sz, *parts);
////////////////
      err = 0;

      if( lparts == 1 )
      {
        (*offs)[0] = (*offs)[1] = 0;
        (*offs)[2] = (long long int)sz;
        *parts = 1; 
        break;
      }
  
      // Set copies of sequence chunks to pagesize
      seqChunks = (long long int)sysconf(_SC_PAGESIZE);

      // Allocate a buffer for inter-partition queries 
      buffer = malloc(seqChunks * sizeof buffer);

      // Compute file partition size for each process
      // Ceiling operation makes sure that summing all partSz is no less than data size
      partSz = (long long int)ceil((double)sz / lparts);
      multiplier = (long long int)floor((double)partSz / seqChunks);
      partSz = seqChunks * multiplier;

      if( partSz == 0 )
      {
        fprintf(stdout, "First Warning: %d...\n", lparts);
        free(buffer);
        err = 2;
      }
      else
      {
      for(i = 0; i < lparts && err == 0; i++)
      {
        // First and middle partitions
        if( i < (lparts - 1) ) 
        { 
          // First partition
          if( i == 0 )
            (*offs)[i*3] = (*offs)[i*3+1] = 0;

          // Middle partitions
          else
          {
            // Compute a page size offset that is not after the end of the previous partition
            prevOffs = (*offs)[(i-1)*3] + (*offs)[(i-1)*3+1] + (*offs)[(i-1)*3+2];
            chunkOffs = (long long int)floor((double)prevOffs/seqChunks);

            // Set array of offsets 
            (*offs)[i*3] = seqChunks * chunkOffs;
            (*offs)[i*3+1] = prevOffs - (*offs)[i*3];
          }

          // Loop backwards through current partition to find beginning of last query
          // Set partition to end at almost-to-last query
          j = 0;
          offset = 0;
          foundSeqBeg = 0;
          while( foundSeqBeg == 0 && err == 0 )
          {
            j++;

            // Read at beginning of chunk from partition
            // Chunks are read beginning at end of partition
            readOffs = (*offs)[i*3] + partSz - (seqChunks * j);
fprintf(stdout, "lparts %d, partSz %lld, mult %ld, readoffs %lld\n", lparts, partSz, multiplier, readOffs);
            if( readOffs < 0 )
            {
              fprintf(stdout, "Warning: too many processes for query file, adjusting...\n");
              free(buffer);
              err = 2;
              break;
            }

            // Set file position at beginning of current chunk
            if( fseeko(fd, readOffs, SEEK_SET) )
              fprintf(stderr, "\nError: failed fseeko() in set offsets\n");
      
            // Read a chunk from the file
            bytesRead = fread(buffer, sizeof(char), seqChunks, fd);
            if( bytesRead != seqChunks )
              fprintf(stdout, "Warning: size of data read does not match, set offsets\n");
   
            // Iterate through chunk and check if found a beginning of query symbol, ">" 
            for(c = seqChunks - 1; c >= 0; c--)
            {
              offset++;
              if( buffer[c] == sym )
              {
                // Set array of offsets
                (*offs)[i*3+2] = partSz - offset - (*offs)[i*3+1];
                if( (*offs)[i*3+2] == 0 )
                {
                  fprintf(stdout, "Warning: too many processes for query file, adjusting...\n");
                  free(buffer);
                  err = 2;
                  break;
                }
                foundSeqBeg = 1;
                break;
              }
            }
          }
        }
        // Last partition
        else 
        {
          // Compute a page size offset that is not after the end of the previous partition
          prevOffs = (*offs)[(i-1)*3] + (*offs)[(i-1)*3+1] + (*offs)[(i-1)*3+2];
          chunkOffs = (long long int)floor((double)prevOffs/seqChunks);
     
fprintf(stdout, "lparts %d, prevOffs %lld, chunkoffs %lld\n", lparts, prevOffs, chunkOffs);
          // Set array of offsets 
          (*offs)[i*3] = seqChunks * chunkOffs;
          (*offs)[i*3+1] = prevOffs - (*offs)[i*3];
          (*offs)[i*3+2] = sz - ((*offs)[i*3] + (*offs)[i*3+1]);
        }
      } // for loop
      }

      if( err == 0 ) free(buffer);

/////////////////
      if( err == 2 )
      {
        // Adjust number of partitions for data processing
        lparts--;
        *offs = realloc(*offs, (lparts * 3) * sizeof offs);
        fprintf(stdout, "Warning: adjusted number of partitions (%d) for data\n", lparts);
        continue;
      }

      if( err != 0 )
      {
        lparts = ERROR;
        free(*offs);
        *offs = NULL;
      }
      
      *parts = lparts;  
      break;
    }  // while loop

  fprintf(stdout, "Finished computing partition offsets\n");

  return 0;
}


// Preprocess query file for memory map offsets
//int setOffsQueryFile(iomap_t *iomap, int procCnt)
int setOffsQueryFile(long long int *fileOffs, FILE *fd, long int fsize, int procCnt)
{
  char *buffer;
  int i;
  int foundSeqBeg;
  long long int readOffs;
  long long int partSz; 
  long long int c;
  long long int j;
  long long int offset;
  long long int seqChunks;
  long int multiplier;
  long long int chunkOffs;
  long long int prevOffs;
  long long int bytesRead;

  // Set copies of sequence chunks to pagesize
  seqChunks = (long long int)sysconf(_SC_PAGESIZE);

  // Allocate a buffer for inter-partition queries
  buffer = (char *)malloc(sizeof(char) * seqChunks);

  // Compute file partition size for each process
  partSz = (long long int)ceil((double)fsize / procCnt);
  multiplier = (long long int)floor((double)partSz / seqChunks);
  partSz = seqChunks * multiplier;

  for(i = 0; i < procCnt; i++)
  {
    // First and middle partitions
    if( i < (procCnt - 1) ) 
    {
      // First partition
      if( i == 0 )
      {
        // Set array of offsets
        fileOffs[i*3] = 0;
        fileOffs[i*3+1] = 0; 
      }
      // Middle partitions
      else
      {
        // Compute a page size offset that is not after the end of the previous partition
        prevOffs = fileOffs[(i-1)*3] + fileOffs[(i-1)*3+1] + fileOffs[(i-1)*3+2];
        chunkOffs = (long long int)floor((double)prevOffs/seqChunks);

        // Set array of offsets 
        fileOffs[i*3] = seqChunks * chunkOffs;
        fileOffs[i*3+1] = prevOffs - fileOffs[i*3];
      }

      // Loop backwards through current partition to find beginning of last query
      // Set partition to end at almost-to-last query
      j = 0;
      offset = 0;
      foundSeqBeg = 0;
      while( foundSeqBeg == 0 )
      {
        j++;

        // Read at beginning of chunk from partition
        // Chunks are read beginning at end of partition
        readOffs = fileOffs[i*3] + partSz - (seqChunks * j);
        if( readOffs < 0 )
        {
          fprintf(stdout, "Warning: too many processes for query file, adjusting...\n");
          free(buffer);
          return 2;
        }

        // Set file position at beginning of current chunk
        if( fseeko(fd, readOffs, SEEK_SET) )
          fprintf(stderr, "\nError: failed fseeko() in set offsets\n");
      
        // Read a chunk from the file
        bytesRead = fread(buffer, sizeof(char), seqChunks, fd);
        if( bytesRead != seqChunks )
          fprintf(stdout, "Warning: size of data read does not match, set offsets\n");
   
        // Iterate through chunk and check if found a beginning of query symbol, ">" 
        for(c = seqChunks - 1; c >= 0; c--)
        {
          offset++;
          if( buffer[c] == '>' )
          {
            // Set array of offsets
            fileOffs[i*3+2] = partSz - offset - fileOffs[i*3+1];
            if( fileOffs[i*3+2] == 0 )
            {
              fprintf(stdout, "Warning: too many processes for query file, adjusting...\n");
              free(buffer);
              return 2;
            }
            foundSeqBeg = 1;
            break;
          }
        }
      }
    }
    // Last partition
    else 
    {
      // Compute a page size offset that is not after the end of the previous partition
      prevOffs = fileOffs[(i-1)*3] + fileOffs[(i-1)*3+1] + fileOffs[(i-1)*3+2];
      chunkOffs = (long long int)floor((double)prevOffs/seqChunks);
     
      // Set array of offsets 
      fileOffs[i*3] = seqChunks * chunkOffs;
      fileOffs[i*3+1] = prevOffs - fileOffs[i*3];
      fileOffs[i*3+2] = fsize - (fileOffs[i*3] + fileOffs[i*3+1]);
    }
  }

  free(buffer);

  return 0;
}


// Adjust number of MPI processes
int adjustMPIProcs(mpi_t *mpi, int worldSz)
{
  // Obtain the group of processes in communicator
  MPI_Group worldGroup;
  MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
  
  // Remove all unnecessary ranks
  MPI_Group newGroup;
  int ranges[1][3] = {{ mpi->procCnt, worldSz-1, 1 }};
  MPI_Group_range_excl(worldGroup, 1, ranges, &newGroup);
  MPI_Group_free(&worldGroup);

  // Create a new communicator
  MPI_Comm_free(&mpi->MPI_MY_WORLD);
  MPI_Comm_create(MPI_COMM_WORLD, newGroup, &mpi->MPI_MY_WORLD);
  if( mpi->MPI_MY_WORLD == MPI_COMM_NULL)
    fprintf(stdout, "Warning: process %d, terminated due to adjust of MPI processes\n", mpi->procRank);
  
  MPI_Group_free(&newGroup);

  return 0;
}


// Determine which processes require input files
// One per node since a file can be opened with multiple file descriptors
int getInputFilesComm(mpi_t *mpi, MPI_Comm *newComm)
{
  int i;
  int j;
  int *nameLen;
  int tmpNameLen;
  char **procNames;
  int *nodeRanks;
  int nodeRanksCnt;
  int uniqueFlag;
  MPI_Status status;
  MPI_Group newGroup;
  MPI_Group currGroup;

  // Allocate data structures
  nodeRanks = (int *)malloc(sizeof(int) * mpi->procCnt);
  nameLen = (int *)malloc(sizeof(int) * mpi->procCnt);
  procNames = (char **)malloc(sizeof(char *) * mpi->procCnt);
  for( i = 0; i < mpi->procCnt; i++)
    procNames[i] = (char *)malloc(sizeof(char) * MPI_MAX_PROCESSOR_NAME);

  // Get the size of all processor names 
  tmpNameLen = mpi->nameLen + 1;
  MPI_Gather(&tmpNameLen, 1, MPI_INT, nameLen, 1, MPI_INT, 0, mpi->MPI_MY_WORLD);

  // Master receives all processor names and groups a single process per distinct node
  // which will receive the input files
  if( mpi->procRank == 0 )
  {
    // Copy all processor names to local buffer
    strncpy(procNames[0], mpi->procName, nameLen[0]);
    for( i = 1; i < mpi->procCnt; i++)
      MPI_Recv(procNames[i], nameLen[i], MPI_CHAR, i, 0, mpi->MPI_MY_WORLD, &status);
   
    // Find the rank of processes that reside in unique compute nodes
    // Master is included by default
    nodeRanks[0] = 0;
    nodeRanksCnt = 1;
    for( i = 1; i < mpi->procCnt; i++)
    {
      uniqueFlag = 1;

      // Check if current processor name is not in the nodes list
      for( j = 0; j < i; j++ )
      {
        if( strncmp(procNames[j], procNames[i], nameLen[i]) == 0 )
        {
          uniqueFlag = 0;
          break;
        }
      }

      // If processor name is unique, add to nodes list
      if( uniqueFlag == 1 )
      {
        fprintf(stdout, "Found unique node %s with rank %d\n", procNames[i], i);  
        nodeRanks[nodeRanksCnt] = i;
        nodeRanksCnt++;
      }
    }
  }
  // Other processes send their processor names to master
  else
    MPI_Send(mpi->procName, tmpNameLen, MPI_CHAR, 0, 0, mpi->MPI_MY_WORLD);

  // Broadcast the distinct nodes list
  MPI_Bcast(&nodeRanksCnt, 1, MPI_INT, 0, mpi->MPI_MY_WORLD);
  MPI_Bcast(nodeRanks, mpi->procCnt, MPI_INT, 0, mpi->MPI_MY_WORLD); 
 
  // Create new group with unique nodes
  MPI_Comm_group(mpi->MPI_MY_WORLD, &currGroup);
  MPI_Group_incl(currGroup, nodeRanksCnt, nodeRanks, &newGroup);

  // Create new communicator with new group 
  MPI_Comm_create(mpi->MPI_MY_WORLD, newGroup, newComm);

  // Free used resources
  MPI_Group_free(&currGroup);
  MPI_Group_free(&newGroup);
  for( i = 0; i < mpi->procCnt; i++)
    free(procNames[i]);
  free(procNames);
  free(nameLen);
  free(nodeRanks);
  
  return 0;
}


// Distribute input files to all processes as needed
int distributeInputFiles(args_t *args, mpi_t *mpi)
{
  long int fsize;      // File size
  FILE *fd;            // File stream
  char *buffer;        // Data buffer
  int allFileFlag;
  int fileFlag;
  int curr_off;
  int curr_sz;
  int next_sz;
  long int bytesRead;
  long int bytesWrite;
  int err;
  MPI_Comm MPI_TMP_WORLD;
  char **inputFiles;
  int inputFileCnt;
  int i;
  int j;

  // Do not distribute for single process programs
  if( mpi->procCnt == 1 )
    return 0;

  // Determine which nodes need the input files
  err = getInputFilesComm(mpi, &MPI_TMP_WORLD);
  if( MPI_TMP_WORLD == MPI_COMM_NULL)
    return 0;

  // Check if master is the only process remaining
  fileFlag = 1;
  MPI_Reduce(&fileFlag, &allFileFlag, 1, MPI_INT, MPI_SUM, 0, MPI_TMP_WORLD);
  if( mpi->procRank == 0 && allFileFlag == 1 )
  {
    MPI_Comm_free(&MPI_TMP_WORLD);
    return 0;
  }

  // Count number of input files, the query file is mandatory
  inputFileCnt = 1;
  if( args->pipeMode != 0 )
    inputFileCnt++;
  if( args->searchMode != 0 )
    inputFileCnt++;

  // Copy filenames into an array
  inputFiles = (char **)malloc(sizeof(char *) * inputFileCnt);
  for( i = 0; i < inputFileCnt; i++ )
    inputFiles[i] = (char *)malloc(sizeof(char) * FILE_LEN);
  
  strncpy(inputFiles[0], args->qf, FILE_LEN);
  if( args->pipeMode != 0 )
    strncpy(inputFiles[1], args->btable, FILE_LEN);
  if( args->searchMode != 0 )
    strncpy(inputFiles[1], args->sf, FILE_LEN);

// Distribute all input files
for( i = 0; i < inputFileCnt; i++ )
{

  // Master process reads input file
  fileFlag = 0;
  if( mpi->procRank == 0 )
  {
    // Open input query file
    fd = fopen(inputFiles[i], "rb");
    if( fd == NULL )
    {
      fprintf(stderr, "\n");
      perror("fopen()");
      fileFlag = ERROR;
    }
    else
    {
      // Set file size
      // Check if file is empty
      struct stat stbuf;
      fstat(fileno(fd), &stbuf);
      fsize = stbuf.st_size;
      if( fsize <= 0L )
      {
        fprintf(stderr, "\nError: query file is empty\n");
        fclose(fd);
        fileFlag = ERROR;
      }
      else
      {
        // Set buffering options, _IOFBF = full buffering of size STRM_BUFSIZ
        setvbuf(fd, NULL, _IOFBF, STRM_BUFSIZ);
        fprintf(stdout, "Master is distributing input file: %s\n", inputFiles[i]);
      }
    }
  }
  // Other processes create file to store data
  else
  {
    // Check if file exists for current process
    fd = fopen(inputFiles[i], "r");
    if( fd != NULL )
    {
      fprintf(stdout, "Process %d detected input file, overwriting\n", mpi->procRank);
      fclose(fd);
    }
      
    // Create file with buffering options
    fd = fopen(inputFiles[i], "wb");
    if( fd == NULL )
      fileFlag = ERROR;
    else
      // Set buffering options, _IOFBF = full buffering of size STRM_BUFSIZ
      setvbuf(fd, NULL, _IOFBF, STRM_BUFSIZ);
  }

  // Check that all processes where able to create file
  MPI_Reduce(&fileFlag, &allFileFlag, 1, MPI_INT, MPI_SUM, 0, MPI_TMP_WORLD);
  if( mpi->procRank == 0 )
  {
    if( allFileFlag != 0 )
      fsize = ERROR;
  }

  // Broadcast the file size
  MPI_Bcast(&fsize, 1, MPI_LONG, 0, MPI_TMP_WORLD);
  if( fsize < 0 )
  {
    for( j = 0; j < inputFileCnt; j++ )
      free(inputFiles[j]);
    free(inputFiles);
    fclose(fd);
    MPI_Comm_free(&MPI_TMP_WORLD);
    return ERROR;
  }

  // Advise to kernel access pattern for file
  posix_fadvise(fileno(fd), 0, MIN(BCAST_LIMIT, fsize), POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
  
  // Allocate a buffer for passing data
  buffer = malloc(sizeof(char) * MIN(BCAST_LIMIT, fsize));
 
  curr_off = 0;
  next_sz = MIN(BCAST_LIMIT, fsize); 
  while (curr_off < fsize)
  {
    curr_sz = next_sz;
    
    // Master reads file and sends
    if( mpi->procRank == 0 )
    {
      // Read the current chunk
      bytesRead = fread(buffer, sizeof(char), curr_sz, fd);
      if( bytesRead != curr_sz )
        fprintf(stdout, "Master did not read chunk size correctly\n");
 
      // We don't need this data cached anymore, but need the next one
      curr_off = curr_off + bytesRead;
      next_sz = MIN(BCAST_LIMIT, fsize - curr_off);
      posix_fadvise(fileno(fd), curr_off, next_sz, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
    }

    // Broadcast chunk 
    MPI_Bcast(buffer, curr_sz, MPI_BYTE, 0, MPI_TMP_WORLD);
   
    // Write data to file
    if( mpi->procRank != 0 )
    {
      bytesWrite = fwrite(buffer, sizeof(char), curr_sz, fd);
      if( bytesWrite != curr_sz )
        fprintf(stdout, "Process did not write chunk size correctly\n");
     
      curr_off = curr_off + bytesWrite;
      next_sz = MIN(BCAST_LIMIT, fsize - curr_off);
      posix_fadvise(fileno(fd), curr_off, next_sz, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);
    }
  }
    
  // Free resources
  free(buffer); 
  fclose(fd);
}

  // Free resources
  for( j = 0; j < inputFileCnt; j++ )
    free(inputFiles[j]);
  free(inputFiles);
  MPI_Comm_free(&MPI_TMP_WORLD);

  return 0;
}




// Preprocess query file for memory map offsets
int setOffs(iomap_t *iomap, mpi_t *mpi)
{
  int procCnt;     // Number of MPI processes
  int err;
 
  iomap->fileOffs = (long long int *)malloc(sizeof(long long int) * mpi->procCnt * 3);
  
  // Only one process 
  if( mpi->procCnt == 1 ) 
  {
    iomap->fileOffs[mpi->procRank*3] = 0;
    iomap->fileOffs[mpi->procRank*3+1] = 0;
    iomap->fileOffs[mpi->procRank*3+2] = iomap->qfsz;
    return 0;
  }

  // Master performs preprocessing of input query file
  // Preprocess query file for memory map offsets
  procCnt = mpi->procCnt;
  if( mpi->procRank == 0 )
  {
    while( 1 )
    {
      // Handle small files with a single process
      if( mpi->procCnt == 1 )
      {
        iomap->fileOffs[mpi->procRank*3] = 0;
        iomap->fileOffs[mpi->procRank*3+1] = 0;
        iomap->fileOffs[mpi->procRank*3+2] = iomap->qfsz;
        break;
      }
 
      // Attempt to distribute offsets considering file size and processes count 
      memset(iomap->fileOffs, 0, sizeof(long long int) * procCnt * 3);
      //err = setOffsQueryFile(iomap, mpi->procCnt);
      err = setOffsQueryFile(iomap->fileOffs, iomap->qfd, iomap->qfsz, mpi->procCnt);
      if( err == 2 )
      {
        // Adjust number of processes for query file processing
        mpi->procCnt--;
        fprintf(stdout, "Warning: adjusted number of processes (%d) for query file\n", mpi->procCnt);
        continue;
      }

      if( err != 0 )
        mpi->procCnt = ERROR;
        
      break;
    }
  }

  // Distribute err flag to check if mastered failed and need to abort program
  err = MPI_Bcast(&mpi->procCnt, 1, MPI_INT, 0, mpi->MPI_MY_WORLD);
  if( err != MPI_SUCCESS || mpi->procCnt == ERROR )
    return ERROR;

  // Adjust MPI processes
  if( mpi->procCnt < procCnt )
  {
    // Adjust MPI processes
    err = adjustMPIProcs(mpi, procCnt);
    if( err != 0 )
      return ERROR;

    // Terminate unnecessary processes
    if( mpi->procRank >= mpi->procCnt )
    {
      free(iomap->fileOffs);
      fclose(iomap->qfd);
      MPI_Finalize();
      exit(0);
    }
  }

  // Synchronize and distribute query file offsets
  err = MPI_Bcast(iomap->fileOffs, procCnt * 3, MPI_LONG_LONG_INT, 0, mpi->MPI_MY_WORLD);
  if( err != MPI_SUCCESS )
    return ERROR;
  
  // Update query mappings with computed offsets
  iomap->qfsz = iomap->fileOffs[mpi->procRank*3+2];

  return 0;
}


////////////////////////////////////////////////////////////////////////////////
//                    Application Entry / High Level Code                     //
////////////////////////////////////////////////////////////////////////////////

// Driver program
int main(int argc, char **argv)
{
  int i;           // Iteration variable
  int err;         // Trap errors
  args_t args;     // Structure for command line options
  iomap_t iomap;   // I/O, memory map control struct
  hits_t hits;     // BLAST table IDs struct
  double start, finish;
  mpi_t mpi;

  // Clear structures
  memset(&args, 0, sizeof(args_t));
  memset(&iomap, 0, sizeof(iomap_t));
  memset(&hits, 0, sizeof(hits_t));
  memset(&mpi, 0, sizeof(mpi_t));

  // Initialize MPI environment
  MPI_Init(&argc, &argv);
  MPI_Comm_dup(MPI_COMM_WORLD, &mpi.MPI_MY_WORLD);
  MPI_Comm_size(mpi.MPI_MY_WORLD, &mpi.procCnt);
  MPI_Comm_rank(mpi.MPI_MY_WORLD, &mpi.procRank);
  MPI_Get_processor_name(mpi.procName, &mpi.nameLen);
  
  // Compute wall time
  start = MPI_Wtime();

  // Parse command line options
  // The loop and the barrier are used simply to prevent the MPI processes from printing concurrently.
  for(i = 0; i < mpi.procCnt; i++)
  {
    MPI_Barrier(mpi.MPI_MY_WORLD);
    if( mpi.procRank == i )
    {
      err = parseCmdline(argc, argv, &args, &mpi);
      if( err != 0 )
      {
        fprintf(stderr, "Error: failed parsing command line options\n\n");
        MPI_Comm_free(&mpi.MPI_MY_WORLD);
        MPI_Finalize();
        return CFGERROR;
      }
    }
  }

#ifdef BCAST_INFILES
  // Distribute input files as necessary
  err = distributeInputFiles(&args, &mpi);
  if( err != 0 )
  {
    fprintf(stderr, "Error: failed distributing input files\n\n");
    MPI_Comm_free(&mpi.MPI_MY_WORLD);
    MPI_Finalize();
    return ERROR;
  }
  MPI_Barrier(mpi.MPI_MY_WORLD);
#endif

  // Open input query file
  err = openQueryFile(args.qf, &iomap);
  if( err != 0 )
  {
    fprintf(stderr, "Error: failed opening query file\n\n");
    MPI_Comm_free(&mpi.MPI_MY_WORLD);
    MPI_Finalize();
    return ERROR;
  }

  // Create array for offsets of memory mappings
  // [i]=file_offset from beginning of file, [i+1]=map offset from file offset, [i+2]=total map size
  err = setOffs(&iomap, &mpi);
  if( err != 0 )
  {
    fprintf(stderr, "Error: failed to set offsets\n\n");
    free(iomap.fileOffs);
    fclose(iomap.qfd);
    MPI_Comm_free(&mpi.MPI_MY_WORLD);
    MPI_Finalize();
    return ERROR;
  }


/// Debug
if( mpi.procRank == 0 )
{
  for( i = 0; i < mpi.procCnt; i++ )
  {
    fprintf(stdout, "setOffs: proc %d = [%lld, %lld, %lld]\n", i, iomap.fileOffs[i*3], iomap.fileOffs[i*3+1], iomap.fileOffs[i*3+2]);
  }

  long long int *offs = NULL;
  int parts = 4;
  char sym = '>';
  err = computePartitionOffsets(&offs, &parts, iomap.qfd, iomap.qfsz, sym);
  if( err != 0 )
  {
    fprintf(stderr, "Error: failed to compute partition offsets\n\n");
    free(offs);
    free(iomap.fileOffs);
    fclose(iomap.qfd);
    MPI_Comm_free(&mpi.MPI_MY_WORLD);
    MPI_Finalize();
    return ERROR;
  }
  
  for( i = 0; i < parts; i++ )
  {
    fprintf(stdout, "computeOffs: part %d = [%lld, %lld, %lld]\n", i, offs[i*3], offs[i*3+1], offs[i*3+2]);
  }
}
///


 
  // Load BLAST table to memory
  hits.pipeMode = args.pipeMode;
  err = loadBlastTable(args.btable, &hits);
  if( err != 0 )   
  {
    fprintf(stderr, "Error: failed loading BLAST table file\n\n");
    free(iomap.fileOffs);
    fclose(iomap.qfd);
    MPI_Comm_free(&mpi.MPI_MY_WORLD);
    MPI_Finalize();
    return ERROR;
  }

  // Load IDs from search file contents for sequence extraction
  hits.searchMode = args.searchMode;
  err = loadSearchIDs(args.sf, &hits);
  if( err != 0 )
  {
    fprintf(stderr, "Error: failed loading search IDs file\n\n");
    free(iomap.fileOffs);
    fclose(iomap.qfd);
    MPI_Comm_free(&mpi.MPI_MY_WORLD);
    MPI_Finalize();
    return ERROR;
  }

  // Partition input file into chunks for query processing
  // Extract sequences from input query file and write to output file
  err = partQueryFile(&args, &iomap, &hits, &mpi);
  if( err != 0 )
  {
    fprintf(stderr, "Error: failed extracting sequences\n\n");
    free(iomap.fileOffs);
    freeHitsMemory(&hits);
    fclose(iomap.qfd);
    MPI_Comm_free(&mpi.MPI_MY_WORLD);
    MPI_Finalize();
    return ERROR;
  }
   
  // Free resources 
  free(iomap.fileOffs);
  freeHitsMemory(&hits);
  fclose(iomap.qfd);

  // Compute wall time
  MPI_Barrier(mpi.MPI_MY_WORLD);
  finish = MPI_Wtime();
  if( mpi.procRank == 0 )
    fprintf(stdout, "Total wall time = %f\n\n", finish - start);
  
  // Finalize MPI environment
  MPI_Comm_free(&mpi.MPI_MY_WORLD);
  MPI_Finalize();

  return 0;
}
