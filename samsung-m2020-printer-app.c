//
// Samsung M2020 series (M2020/M2021/M2022/M2026...) Printer Application.
//
// A PAPPL-based IPP Everywhere / AirPrint Printer Application that drives
// USB-only Samsung QPDL (SPL) laser printers through the SpliX
// "rastertoqpdl" filter.
//
// Pipeline:
//
//   IPP client (macOS, iOS, Linux, Windows)
//     -> PAPPL (PWG raster / URF / PNG / JPEG, dithered to 1-bit black)
//     -> this driver: writes a CUPS raster stream (K, 1 bit, compression 17)
//     -> SpliX rastertoqpdl + m2020.ppd: converts it to QPDL v3
//     -> PAPPL USB device (libusb)
//
// Copyright (c) 2026.  Licensed under the Apache License 2.0.
// SpliX (rastertoqpdl) and the generated PPD file are GPL-2.0 and are run
// as a separate program.
//

#include <pappl/pappl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef VERSION
#  define VERSION "0.1.0"
#endif // !VERSION
#ifndef M2K_FILTER
#  define M2K_FILTER "/usr/local/libexec/samsung-m2020-printer-app/rastertoqpdl"
#endif // !M2K_FILTER
#ifndef M2K_PPD
#  define M2K_PPD "/usr/local/share/samsung-m2020-printer-app/m2020.ppd"
#endif // !M2K_PPD

//
// Constants...
//

#define M2K_DRIVER	"samsung_m2020"	// Driver name
#define M2K_COMPRESSION	17		// QPDL algorithm 0x11 (from m2020.ppd)
#define M2K_MARGIN	441		// 12.5pt hardware margins in 1/100mm
#define M2K_USB_BLOCK	8192		// USB write block (multiple of 512, same as usblp)


//
// Types...
//

typedef struct m2k_media_s		// PWG <-> PPD media name mapping
{
  const char	*pwg,			// PWG self-describing name
		*ppd;			// PPD PageSize keyword
} m2k_media_t;

typedef struct m2k_job_s		// Per-job driver state
{
  char		rasfile[1024];		// Temporary CUPS raster file
  int		rasfd;			// Raster file descriptor
  cups_raster_t	*ras;			// Raster stream
  cups_page_header2_t header;		// Current page header
  char		ppdopts[1024];		// PPD options for rastertoqpdl
} m2k_job_t;


//
// Local globals...
//

static pappl_pr_driver_t m2k_drivers[] =
{					// Supported drivers
  { M2K_DRIVER, "Samsung M2020 Series (SpliX QPDL)", "MFG:Samsung;MDL:M2020 Series;", NULL }
};

static const m2k_media_t m2k_media[] =	// Media sizes, also present in m2020.ppd
{
  { "iso_a4_210x297mm",           "A4" },
  { "na_letter_8.5x11in",         "Letter" },
  { "na_legal_8.5x14in",          "Legal" },
  { "na_executive_7.25x10.5in",   "Executive" },
  { "om_folio_210x330mm",         "Folio" },
  { "iso_a5_148x210mm",           "A5" },
  { "iso_a6_105x148mm",           "A6" },
  { "jis_b5_182x257mm",           "B5" },
  { "jis_b6_128x182mm",           "B6" },
  { "iso_b5_176x250mm",           "EnvISOB5" },
  { "na_number-10_4.125x9.5in",   "Env10" },
  { "na_monarch_3.875x7.5in",     "Monarch" },
  { "iso_c5_162x229mm",           "C5" },
  { "iso_c6_114x162mm",           "C6" },
  { "iso_dl_110x220mm",           "DL" },
  { "jpn_hagaki_100x148mm",       "Postcard" }
};

static const m2k_media_t m2k_types[] =	// Media types -> PPD MediaType
{
  { "stationery",             "NORMAL" },
  { "stationery-lightweight", "THIN" },
  { "stationery-heavyweight", "THICK" },
  { "stationery-recycled",    "RECYCLED" },
  { "stationery-preprinted",  "USED" },
  { "stationery-colored",     "COLOR" },
  { "cardstock",              "CARD" },
  { "labels",                 "LABEL" },
  { "envelope",               "ENV" },
  { "transparency",           "OHP" }
};


//
// Local functions...
//

static const char *m2k_autoadd(const char *device_info, const char *device_uri, const char *device_id, void *data);
static bool	m2k_driver_cb(pappl_system_t *system, const char *driver_name, const char *device_uri, const char *device_id, pappl_pr_driver_data_t *driver_data, ipp_t **driver_attrs, void *data);
static void	m2k_free_job(pappl_job_t *job, m2k_job_t *mj);
static void	m2k_identify(pappl_printer_t *printer, pappl_identify_actions_t actions, const char *message);
static const char *m2k_lookup(const m2k_media_t *table, size_t count, const char *pwg, const char *defval);
static bool	m2k_printfile(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device);
static bool	m2k_rendjob(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device);
static bool	m2k_rendpage(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned page);
static bool	m2k_rstartjob(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device);
static bool	m2k_rstartpage(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned page);
static bool	m2k_rwriteline(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned y, const unsigned char *line);
static bool	m2k_run_filter(pappl_job_t *job, m2k_job_t *mj, pappl_device_t *device);
static void	m2k_sanitize(char *dst, const char *src, size_t dstsize);
static bool	m2k_status(pappl_printer_t *printer);


//
// 'main()' - Main entry for the printer application.
//

int					// O - Exit status
main(int  argc,				// I - Number of command-line arguments
     char *argv[])			// I - Command-line arguments
{
  return (papplMainloop(argc, argv,
                        VERSION,
                        "Samsung M2020 Printer Application &mdash; built on <a href=\"https://www.msweet.org/pappl\">PAPPL</a> and <a href=\"https://github.com/OpenPrinting/splix\">SpliX</a>.",
                        (int)(sizeof(m2k_drivers) / sizeof(m2k_drivers[0])),
                        m2k_drivers, m2k_autoadd, m2k_driver_cb,
                        /*subcmd_name*/NULL, /*subcmd_cb*/NULL,
                        /*system_cb*/NULL,
                        /*usage_cb*/NULL,
                        /*data*/NULL));
}


//
// 'm2k_autoadd()' - Pick the driver for an IEEE-1284 device ID.
//

static const char *			// O - Driver name or `NULL` for none
m2k_autoadd(const char *device_info,	// I - Device name (not used)
            const char *device_uri,	// I - Device URI (not used)
            const char *device_id,	// I - IEEE-1284 device ID
            void       *data)		// I - Callback data (not used)
{
  const char	*ret = NULL;		// Return value
  int		num_did;		// Number of device ID key/value pairs
  cups_option_t	*did;			// Device ID key/value pairs
  const char	*mfg, *mdl;		// Manufacturer and model


  (void)device_info;
  (void)device_uri;
  (void)data;

  num_did = papplDeviceParseID(device_id, &did);

  if ((mfg = cupsGetOption("MANUFACTURER", num_did, did)) == NULL)
    mfg = cupsGetOption("MFG", num_did, did);
  if ((mdl = cupsGetOption("MODEL", num_did, did)) == NULL)
    mdl = cupsGetOption("MDL", num_did, did);

  // Samsung M2020/M2021/M2022/M2026 (all report "M2020 Series")...
  if (mfg && mdl && !strncasecmp(mfg, "Samsung", 7) && (strstr(mdl, "M2020") || strstr(mdl, "M202")))
    ret = M2K_DRIVER;

  cupsFreeOptions(num_did, did);

  return (ret);
}


//
// 'm2k_driver_cb()' - Fill in the driver data.
//

static bool				// O - `true` on success, `false` on failure
m2k_driver_cb(
    pappl_system_t         *system,	// I - System (not used)
    const char             *driver_name,// I - Driver name
    const char             *device_uri,	// I - Device URI (not used)
    const char             *device_id,	// I - IEEE-1284 device ID (not used)
    pappl_pr_driver_data_t *driver_data,// O - Driver data
    ipp_t                  **driver_attrs,
					// O - Driver attributes (not used)
    void                   *data)	// I - Callback data (not used)
{
  int		i;			// Looping var
  static const pappl_dither_t dither =	// 16x16 clustered-dot ordered dither
  {
    {   0, 128,  32, 160,   8, 136,  40, 168,   2, 130,  34, 162,  10, 138,  42, 170 },
    { 192,  64, 224,  96, 200,  72, 232, 104, 194,  66, 226,  98, 202,  74, 234, 106 },
    {  48, 176,  16, 144,  56, 184,  24, 152,  50, 178,  18, 146,  58, 186,  26, 154 },
    { 240, 112, 208,  80, 248, 120, 216,  88, 242, 114, 210,  82, 250, 122, 218,  90 },
    {  12, 140,  44, 172,   4, 132,  36, 164,  14, 142,  46, 174,   6, 134,  38, 166 },
    { 204,  76, 236, 108, 196,  68, 228, 100, 206,  78, 238, 110, 198,  70, 230, 102 },
    {  60, 188,  28, 156,  52, 180,  20, 148,  62, 190,  30, 158,  54, 182,  22, 150 },
    { 252, 124, 220,  92, 244, 116, 212,  84, 254, 126, 222,  94, 246, 118, 214,  86 },
    {   3, 131,  35, 163,  11, 139,  43, 171,   1, 129,  33, 161,   9, 137,  41, 169 },
    { 195,  67, 227,  99, 203,  75, 235, 107, 193,  65, 225,  97, 201,  73, 233, 105 },
    {  51, 179,  19, 147,  59, 187,  27, 155,  49, 177,  17, 145,  57, 185,  25, 153 },
    { 243, 115, 211,  83, 251, 123, 219,  91, 241, 113, 209,  81, 249, 121, 217,  89 },
    {  15, 143,  47, 175,   7, 135,  39, 167,  13, 141,  45, 173,   5, 133,  37, 165 },
    { 207,  79, 239, 111, 199,  71, 231, 103, 205,  77, 237, 109, 197,  69, 229, 101 },
    {  63, 191,  31, 159,  55, 183,  23, 151,  61, 189,  29, 157,  53, 181,  21, 149 },
    { 255, 127, 223,  95, 247, 119, 215,  87, 253, 125, 221,  93, 245, 117, 213,  85 }
  };


  (void)system;
  (void)device_uri;
  (void)device_id;
  (void)driver_attrs;
  (void)data;

  if (strcmp(driver_name, M2K_DRIVER))
    return (false);

  // Callbacks...
  driver_data->identify_cb   = m2k_identify;
  driver_data->printfile_cb  = m2k_printfile;
  driver_data->rendjob_cb    = m2k_rendjob;
  driver_data->rendpage_cb   = m2k_rendpage;
  driver_data->rstartjob_cb  = m2k_rstartjob;
  driver_data->rstartpage_cb = m2k_rstartpage;
  driver_data->rwriteline_cb = m2k_rwriteline;
  driver_data->status_cb     = m2k_status;

  // Raw QPDL streams can be sent directly (useful for debugging)...
  driver_data->format = "application/vnd.samsung-qpdl";

  papplCopyString(driver_data->make_and_model, "Samsung M2020 Series", sizeof(driver_data->make_and_model));

  driver_data->ppm             = 20;
  driver_data->orient_default  = IPP_ORIENT_NONE;
  driver_data->quality_default = IPP_QUALITY_NORMAL;

  // Dithering: PAPPL converts gray input to 1-bit black for us...
  memcpy(driver_data->gdither, dither, sizeof(driver_data->gdither));
  memcpy(driver_data->pdither, dither, sizeof(driver_data->pdither));

  driver_data->raster_types      = PAPPL_PWG_RASTER_TYPE_BLACK_1 | PAPPL_PWG_RASTER_TYPE_BLACK_8 | PAPPL_PWG_RASTER_TYPE_SGRAY_8;
  driver_data->force_raster_type = PAPPL_PWG_RASTER_TYPE_BLACK_1;
  driver_data->color_supported   = PAPPL_COLOR_MODE_MONOCHROME | PAPPL_COLOR_MODE_BI_LEVEL;
  driver_data->color_default     = PAPPL_COLOR_MODE_MONOCHROME;

  // Resolutions supported by the M2020 PPD: 600dpi always, 1200dpi only on
  // request (PAPPL would otherwise pick 1200dpi for print-quality=normal)...
  driver_data->num_resolution  = 1;
  driver_data->x_resolution[0] = driver_data->y_resolution[0] = 600;
  driver_data->x_default       = driver_data->y_default = 600;

  if (getenv("M2020_ENABLE_1200DPI") && atoi(getenv("M2020_ENABLE_1200DPI")) > 0)
  {
    driver_data->num_resolution  = 2;
    driver_data->x_resolution[1] = driver_data->y_resolution[1] = 1200;
  }

  // Toner density (Light/Medium/Dark) via printer-darkness-configured...
  driver_data->darkness_supported  = 3;
  driver_data->darkness_configured = 50;
  driver_data->darkness_default    = 0;

  // Media...
  driver_data->num_media = (int)(sizeof(m2k_media) / sizeof(m2k_media[0]));
  for (i = 0; i < driver_data->num_media; i ++)
    driver_data->media[i] = m2k_media[i].pwg;

  driver_data->left_right = M2K_MARGIN;
  driver_data->bottom_top = M2K_MARGIN;

  driver_data->num_source = 2;
  driver_data->source[0]  = "auto";
  driver_data->source[1]  = "manual";

  driver_data->num_type = (int)(sizeof(m2k_types) / sizeof(m2k_types[0]));
  for (i = 0; i < driver_data->num_type; i ++)
    driver_data->type[i] = m2k_types[i].pwg;

  for (i = 0; i < driver_data->num_source; i ++)
  {
    papplCopyString(driver_data->media_ready[i].size_name, "iso_a4_210x297mm", sizeof(driver_data->media_ready[i].size_name));
    papplCopyString(driver_data->media_ready[i].source, driver_data->source[i], sizeof(driver_data->media_ready[i].source));
    papplCopyString(driver_data->media_ready[i].type, "stationery", sizeof(driver_data->media_ready[i].type));
    driver_data->media_ready[i].bottom_margin = driver_data->media_ready[i].top_margin = M2K_MARGIN;
    driver_data->media_ready[i].left_margin = driver_data->media_ready[i].right_margin = M2K_MARGIN;
    driver_data->media_ready[i].size_width  = 21000;
    driver_data->media_ready[i].size_length = 29700;
  }
  driver_data->media_default = driver_data->media_ready[0];

  // One-sided only (the M2020 has no duplexer)...
  driver_data->sides_supported = PAPPL_SIDES_ONE_SIDED;
  driver_data->sides_default   = PAPPL_SIDES_ONE_SIDED;

  return (true);
}


//
// 'm2k_free_job()' - Free per-job state.
//

static void
m2k_free_job(pappl_job_t *job,		// I - Job
             m2k_job_t   *mj)		// I - Driver job data
{
  if (!mj)
    return;

  if (mj->ras)
    cupsRasterClose(mj->ras);
  if (mj->rasfd >= 0)
    close(mj->rasfd);
  if (mj->rasfile[0])
  {
    if (getenv("M2020_KEEP_RASTER"))
      papplLogJob(job, PAPPL_LOGLEVEL_INFO, "Keeping CUPS raster file '%s'.", mj->rasfile);
    else
      unlink(mj->rasfile);
  }

  free(mj);
  papplJobSetData(job, NULL);
}


//
// 'm2k_identify()' - Identify the printer (nothing to do on this model).
//

static void
m2k_identify(
    pappl_printer_t          *printer,	// I - Printer
    pappl_identify_actions_t actions,	// I - Actions
    const char               *message)	// I - Message, if any
{
  (void)actions;
  (void)message;

  papplLogPrinter(printer, PAPPL_LOGLEVEL_INFO, "Identify-Printer is not supported by this model.");
}


//
// 'm2k_lookup()' - Map a PWG keyword to a PPD keyword.
//

static const char *			// O - PPD keyword
m2k_lookup(const m2k_media_t *table,	// I - Table
           size_t            count,	// I - Number of entries
           const char        *pwg,	// I - PWG keyword
           const char        *defval)	// I - Default value
{
  size_t	i;			// Looping var


  if (pwg && *pwg)
  {
    for (i = 0; i < count; i ++)
      if (!strcmp(table[i].pwg, pwg))
        return (table[i].ppd);
  }

  return (defval);
}


//
// 'm2k_printfile()' - Send a raw QPDL file to the printer.
//

static bool				// O - `true` on success, `false` on failure
m2k_printfile(
    pappl_job_t        *job,		// I - Job
    pappl_pr_options_t *options,	// I - Job options (not used)
    pappl_device_t     *device)		// I - Output device
{
  int		fd;			// Input file
  ssize_t	bytes;			// Bytes read
  char		buffer[65536];		// Copy buffer


  (void)options;

  if ((fd = open(papplJobGetFilename(job), O_RDONLY)) < 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to open print file '%s': %s", papplJobGetFilename(job), strerror(errno));
    return (false);
  }

  while ((bytes = read(fd, buffer, sizeof(buffer))) > 0)
  {
    if (papplDeviceWrite(device, buffer, (size_t)bytes) < 0)
    {
      papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to send %d bytes to printer.", (int)bytes);
      close(fd);
      return (false);
    }
  }

  close(fd);
  papplDeviceFlush(device);

  return (true);
}


//
// 'm2k_rendjob()' - End a raster job: convert with SpliX and send to the printer.
//

static bool				// O - `true` on success, `false` on failure
m2k_rendjob(
    pappl_job_t        *job,		// I - Job
    pappl_pr_options_t *options,	// I - Job options (not used)
    pappl_device_t     *device)		// I - Output device
{
  m2k_job_t	*mj = (m2k_job_t *)papplJobGetData(job);
					// Driver job data
  bool		ret = false;		// Return value


  (void)options;

  if (!mj)
    return (false);

  // Close the raster stream so the file is complete...
  cupsRasterClose(mj->ras);
  mj->ras = NULL;
  close(mj->rasfd);
  mj->rasfd = -1;

  if (!papplJobIsCanceled(job))
    ret = m2k_run_filter(job, mj, device);

  m2k_free_job(job, mj);

  return (ret);
}


//
// 'm2k_rendpage()' - End a page.
//

static bool				// O - `true` on success, `false` on failure
m2k_rendpage(
    pappl_job_t        *job,		// I - Job (not used)
    pappl_pr_options_t *options,	// I - Job options (not used)
    pappl_device_t     *device,		// I - Output device (not used)
    unsigned           page)		// I - Page number (not used)
{
  (void)job;
  (void)options;
  (void)device;
  (void)page;

  return (true);
}


//
// 'm2k_rstartjob()' - Start a raster job.
//

static bool				// O - `true` on success, `false` on failure
m2k_rstartjob(
    pappl_job_t        *job,		// I - Job
    pappl_pr_options_t *options,	// I - Job options
    pappl_device_t     *device)		// I - Output device (not used)
{
  m2k_job_t	*mj;			// Driver job data
  const char	*tmpdir,		// Temporary directory
		*pagesize,		// PPD PageSize
		*slot,			// PPD InputSlot
		*type,			// PPD MediaType
		*density,		// PPD TonerDensity
		*econo;			// PPD EconoMode
  int		darkness;		// Effective darkness 0-100


  (void)device;

  if ((mj = (m2k_job_t *)calloc(1, sizeof(m2k_job_t))) == NULL)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to allocate memory for job.");
    return (false);
  }

  mj->rasfd = -1;
  papplJobSetData(job, mj);

  // Create the temporary raster file...
  if ((tmpdir = getenv("TMPDIR")) == NULL || !*tmpdir)
    tmpdir = "/tmp";

  snprintf(mj->rasfile, sizeof(mj->rasfile), "%s/m2020-job%d-XXXXXX", tmpdir, papplJobGetID(job));

  if ((mj->rasfd = mkstemp(mj->rasfile)) < 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to create temporary file '%s': %s", mj->rasfile, strerror(errno));
    mj->rasfile[0] = '\0';
    m2k_free_job(job, mj);
    return (false);
  }

  if ((mj->ras = cupsRasterOpen(mj->rasfd, CUPS_RASTER_WRITE_COMPRESSED)) == NULL)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to open raster stream: %s", cupsLastErrorString());
    m2k_free_job(job, mj);
    return (false);
  }

  // Map IPP options to the PPD options SpliX understands...
  pagesize = m2k_lookup(m2k_media, sizeof(m2k_media) / sizeof(m2k_media[0]), options->media.size_name, NULL);
  if (!pagesize)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_WARN, "Unsupported media '%s', using A4.", options->media.size_name);
    pagesize = "A4";
  }

  slot = strcmp(options->media.source, "manual") ? "Auto" : "Manual";
  type = m2k_lookup(m2k_types, sizeof(m2k_types) / sizeof(m2k_types[0]), options->media.type, "OFF");

  darkness = options->darkness_configured + options->print_darkness;
  if (darkness < 34)
    density = "1";
  else if (darkness < 67)
    density = "3";
  else
    density = "5";

  econo = options->print_quality == IPP_QUALITY_DRAFT ? "ON" : "0";

  snprintf(mj->ppdopts, sizeof(mj->ppdopts), "PageSize=%s InputSlot=%s MediaType=%s Resolution=%ddpi TonerDensity=%s EconoMode=%s ColorModel=Gray", pagesize, slot, type, options->printer_resolution[0] >= 1200 ? 1200 : 600, density, econo);

  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "SpliX options: %s", mj->ppdopts);

  return (true);
}


//
// 'm2k_rstartpage()' - Start a page.
//

static bool				// O - `true` on success, `false` on failure
m2k_rstartpage(
    pappl_job_t        *job,		// I - Job
    pappl_pr_options_t *options,	// I - Job options
    pappl_device_t     *device,		// I - Output device (not used)
    unsigned           page)		// I - Page number (not used)
{
  m2k_job_t	*mj = (m2k_job_t *)papplJobGetData(job);
					// Driver job data


  (void)device;
  (void)page;

  if (!mj || !mj->ras)
    return (false);

  if (options->header.cupsBitsPerPixel != 1 || options->header.cupsColorSpace != CUPS_CSPACE_K)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unexpected raster format (%u bpp, color space %u).", options->header.cupsBitsPerPixel, options->header.cupsColorSpace);
    return (false);
  }

  // SpliX wants the CUPS raster header the PPD would have produced...
  mj->header                 = options->header;
  mj->header.cupsCompression = M2K_COMPRESSION;
  mj->header.NumCopies       = 1;	// PAPPL generates the copies
  mj->header.Margins[0]      = 12;	// Same as CUPS would set from the
  mj->header.Margins[1]      = 12;	// PPD's 12.5pt ImageableArea
  mj->header.MediaPosition   = strcmp(options->media.source, "manual") ? 1 : 2;

  if (!cupsRasterWriteHeader2(mj->ras, &mj->header))
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to write raster header: %s", cupsLastErrorString());
    return (false);
  }

  return (true);
}


//
// 'm2k_rwriteline()' - Write a raster line (1-bit, 1 = black).
//

static bool				// O - `true` on success, `false` on failure
m2k_rwriteline(
    pappl_job_t         *job,		// I - Job
    pappl_pr_options_t  *options,	// I - Job options (not used)
    pappl_device_t      *device,	// I - Output device (not used)
    unsigned            y,		// I - Line number (not used)
    const unsigned char *line)		// I - Line
{
  m2k_job_t	*mj = (m2k_job_t *)papplJobGetData(job);
					// Driver job data


  (void)options;
  (void)device;
  (void)y;

  if (!mj || !mj->ras)
    return (false);

  return (cupsRasterWritePixels(mj->ras, (unsigned char *)line, mj->header.cupsBytesPerLine) == mj->header.cupsBytesPerLine);
}


//
// 'm2k_run_filter()' - Run SpliX rastertoqpdl and copy its output to the device.
//

static bool				// O - `true` on success, `false` on failure
m2k_run_filter(pappl_job_t    *job,	// I - Job
               m2k_job_t      *mj,	// I - Driver job data
               pappl_device_t *device)	// I - Output device
{
  const char	*filter,		// rastertoqpdl path
		*ppd;			// PPD path
  char		jobid[32],		// Job ID string
		user[256],		// Sanitized user name
		title[256],		// Sanitized job title
		ppdenv[1100],		// PPD=... environment variable
		errfile[1024],		// stderr capture file
		*argv[7],		// Filter arguments
		*envp[5];		// Filter environment
  int		infd = -1,		// Raster input
		errfd = -1,		// stderr capture
		outpipe[2] = { -1, -1 },// Filter output pipe
		status = 0;		// Exit status
  pid_t		pid;			// Filter process
  posix_spawn_file_actions_t fa;	// Spawn file actions
  ssize_t	bytes;			// Bytes read
  size_t	total = 0;		// Total bytes sent
  bool		ok = true;		// Write status
  char		buffer[65536];		// Copy buffer
  unsigned char	block[M2K_USB_BLOCK];	// Full-size USB write block
  size_t	blockused = 0;		// Bytes used in block
  const char	*tmpdir;		// Temporary directory
  FILE		*errfp;			// stderr capture stream


  if ((filter = getenv("M2020_FILTER")) == NULL || !*filter)
    filter = M2K_FILTER;
  if ((ppd = getenv("M2020_PPD")) == NULL || !*ppd)
    ppd = M2K_PPD;

  if (access(filter, X_OK))
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "SpliX filter '%s' not found or not executable.", filter);
    return (false);
  }
  if (access(ppd, R_OK))
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "PPD file '%s' not found.", ppd);
    return (false);
  }

  snprintf(jobid, sizeof(jobid), "%d", papplJobGetID(job));
  m2k_sanitize(user, papplJobGetUsername(job) ? papplJobGetUsername(job) : "guest", sizeof(user));
  m2k_sanitize(title, papplJobGetName(job) ? papplJobGetName(job) : "Untitled", sizeof(title));
  snprintf(ppdenv, sizeof(ppdenv), "PPD=%s", ppd);

  argv[0] = "rastertoqpdl";
  argv[1] = jobid;
  argv[2] = user;
  argv[3] = title;
  argv[4] = "1";
  argv[5] = mj->ppdopts;
  argv[6] = NULL;

  envp[0] = ppdenv;
  envp[1] = "PATH=/usr/bin:/bin";
  envp[2] = "LANG=C";
  envp[3] = "LC_ALL=C";
  envp[4] = NULL;

  if ((infd = open(mj->rasfile, O_RDONLY)) < 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to reopen raster file: %s", strerror(errno));
    return (false);
  }

  if ((tmpdir = getenv("TMPDIR")) == NULL || !*tmpdir)
    tmpdir = "/tmp";
  snprintf(errfile, sizeof(errfile), "%s/m2020-err%s-XXXXXX", tmpdir, jobid);
  if ((errfd = mkstemp(errfile)) < 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to create temporary file: %s", strerror(errno));
    close(infd);
    return (false);
  }

  if (pipe(outpipe))
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to create pipe: %s", strerror(errno));
    close(infd);
    close(errfd);
    unlink(errfile);
    return (false);
  }

  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, infd, 0);
  posix_spawn_file_actions_adddup2(&fa, outpipe[1], 1);
  posix_spawn_file_actions_adddup2(&fa, errfd, 2);
  posix_spawn_file_actions_addclose(&fa, outpipe[0]);

  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Running %s %s %s \"%s\" 1 \"%s\" (%s)", filter, jobid, user, title, mj->ppdopts, ppdenv);

  status = posix_spawn(&pid, filter, &fa, NULL, argv, envp);
  posix_spawn_file_actions_destroy(&fa);

  close(infd);
  close(outpipe[1]);

  if (status)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to run '%s': %s", filter, strerror(status));
    close(outpipe[0]);
    close(errfd);
    unlink(errfile);
    return (false);
  }

  // Copy QPDL to the printer as it is produced...
  while ((bytes = read(outpipe[0], buffer, sizeof(buffer))) != 0)
  {
    if (bytes < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      break;
    }

    if (!ok)
      continue;				// Drain the pipe so the filter can exit

    if (papplJobIsCanceled(job))
    {
      kill(pid, SIGTERM);
      ok = false;
      continue;
    }

    // The printer's firmware does not tolerate short USB packets in the middle
    // of the stream (they show up as "SPL ERROR - Incomplete Session by time
    // out"), so repack the data into full M2K_USB_BLOCK byte blocks exactly
    // like the Linux "usblp" driver does; only the very last block may be
    // short.
    {
      const unsigned char *src = (const unsigned char *)buffer;
					// Source pointer
      size_t	left = (size_t)bytes,	// Bytes left to repack
		n;			// Bytes to copy

      while (left > 0)
      {
        n = sizeof(block) - blockused;
        if (n > left)
          n = left;

        memcpy(block + blockused, src, n);
        blockused += n;
        src       += n;
        left      -= n;

        if (blockused == sizeof(block))
        {
          if (papplDeviceWrite(device, block, sizeof(block)) < 0)
          {
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to send %d bytes to printer.", (int)sizeof(block));
            kill(pid, SIGTERM);
            ok = false;
            break;
          }

          blockused = 0;
        }
      }
    }

    total += (size_t)bytes;
  }

  close(outpipe[0]);

  // Send the final (possibly short) block...
  if (ok && blockused > 0 && papplDeviceWrite(device, block, blockused) < 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to send last %d bytes to printer.", (int)blockused);
    ok = false;
  }

  papplDeviceFlush(device);

  while (waitpid(pid, &status, 0) < 0 && errno == EINTR);

  // Relay SpliX messages to the job log...
  if ((errfp = fdopen(errfd, "r")) != NULL)
  {
    char line[1024], *ptr;		// Line from stderr

    rewind(errfp);
    while (fgets(line, sizeof(line), errfp))
    {
      if ((ptr = line + strlen(line) - 1) >= line && *ptr == '\n')
        *ptr = '\0';

      if (!strncmp(line, "ERROR:", 6))
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "SpliX: %s", line + 6);
      else if (!strncmp(line, "WARNING:", 8))
        papplLogJob(job, PAPPL_LOGLEVEL_WARN, "SpliX: %s", line + 8);
      else if (line[0])
        papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "SpliX: %s", line);
    }
    fclose(errfp);
  }
  else
    close(errfd);

  unlink(errfile);

  if (!WIFEXITED(status) || WEXITSTATUS(status))
  {
    if (WIFSIGNALED(status))
      papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "SpliX crashed with signal %d.", WTERMSIG(status));
    else
      papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "SpliX failed with exit status %d.", WEXITSTATUS(status));
    ok = false;
  }

  papplLogJob(job, PAPPL_LOGLEVEL_INFO, "Sent %lu bytes of QPDL to the printer.", (unsigned long)total);

  return (ok && total > 0);
}


//
// 'm2k_sanitize()' - Make a string safe for PJL ("@PJL JOB NAME=\"...\"").
//

static void
m2k_sanitize(char       *dst,		// I - Destination
             const char *src,		// I - Source
             size_t     dstsize)	// I - Size of destination
{
  char	*end = dst + dstsize - 1;	// End of destination


  while (*src && dst < end)
  {
    unsigned char ch = (unsigned char)*src++;

    *dst++ = (ch < ' ' || ch >= 0x7f || ch == '\"' || ch == '\\') ? '_' : (char)ch;
  }

  *dst = '\0';
}


//
// 'm2k_status()' - Update printer status (USB-only GDI printer, nothing to query).
//

static bool				// O - `true` on success
m2k_status(pappl_printer_t *printer)	// I - Printer
{
  (void)printer;

  return (true);
}
