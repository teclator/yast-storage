/*
  Textdomain    "storage"
*/

#include <dirent.h>
#include <glob.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <pwd.h>
#include <config.h>
#include <signal.h>
#include <sys/utsname.h>
#include <errno.h>

#include <set>
#include <fstream>
#include <sstream>
#include <iostream>
#include <boost/algorithm/string.hpp>

#include "y2storage/Storage.h"
#include "y2storage/StorageTmpl.h"
#include "y2storage/AppUtil.h"
#include "y2storage/SystemCmd.h"
#include "y2storage/Disk.h"
#include "y2storage/Dasd.h"
#include "y2storage/MdCo.h"
#include "y2storage/DmCo.h"
#include "y2storage/LoopCo.h"
#include "y2storage/LvmVg.h"
#include "y2storage/IterPair.h"
#include "y2storage/ProcMounts.h"
#include "y2storage/ProcPart.h"
#include "y2storage/EtcFstab.h"
#include "y2storage/AsciiFile.h"
#include "y2storage/StorageDefines.h"

using namespace std;
using namespace storage;


void
Storage::initDefaultLogger ()
{
    string path;
    string file;
    if (geteuid ())
    {
	struct passwd* pw = getpwuid (geteuid ());
	if (pw)
	{
	    path = pw->pw_dir;
	    file = ".y2log";
	}
	else
	{
	    path = "/";
	    file = "y2log";
	}
    }
    else
    {
	path = "/var/log/YaST2";
	file = "y2log";
    }

    storage::createLogger("libstorage", "default", path, file);
}


Storage::Storage( bool ronly, bool tmode, bool autodetec )
    : lock(ronly, tmode), readonly(ronly), testmode(tmode), initialized(false),
      autodetect(autodetec)
{
    y2mil("constructed Storage ronly:" << ronly << " testmode:" << testmode <<
	  " autodetect:" << autodetect);
    y2mil("package string \"" PACKAGE_STRING "\"");
    char * tenv = getenv( "YAST_IS_RUNNING" );
    inst_sys = tenv!=NULL && strcmp(tenv,"instsys")==0;
    root_mounted = !inst_sys;
    efiboot = false;
    hald_pid = 0;
    if( !testmode )
	testmode = getenv( "YAST2_STORAGE_TMODE" )!=NULL;
    max_log_num = 5;
    tenv = getenv( "Y2MAXLOGNUM" );
    logdir = "/var/log/YaST2";
    if( tenv!=0 )
	string(tenv) >> max_log_num;
    y2mil("instsys:" << inst_sys << " testmode:" << testmode << " autodetect:" << autodetect <<
	  " log:" << max_log_num);
    progress_bar_cb = NULL;
    install_info_cb = NULL;
    info_popup_cb = NULL;
    yesno_popup_cb = NULL;
    recursiveRemove = false;
    zeroNewPartitions = false;
    defaultMountBy = MOUNTBY_ID;
    detectMounted = true;

    logSystemInfo();
}


void
Storage::logSystemInfo() const
{
    ifstream File( "/proc/version" );
    classic(File);
    string line;
    getline( File, line );
    File.close();
    y2mil( "kernel version:" << line );
}


void
Storage::initialize()
    {
    initialized = true;
    char tbuf[100];
    strncpy( tbuf, "/tmp/liby2storageXXXXXX", sizeof(tbuf)-1 );
    if( mkdtemp( tbuf )==NULL )
	{
	cerr << "tmpdir creation " << tbuf << " failed. Aborting..." << endl;
	exit(1);
	}
    else
	{
	tempdir = tbuf;
	rmdir( tempdir.c_str() );
	}
    if( access( "/etc/sysconfig/storage", R_OK )==0 )
    {
	AsciiFile sc( "/etc/sysconfig/storage" );
	Regex r('^' + Regex::ws + "DEVICE_NAMES" + '=' + "(['\"]?)([^'\"]*)\\1" + Regex::ws + '$');
	int line = sc.find( 0, r );
	if( line >= 0 )
	{
	    string val = boost::to_lower_copy(r.cap(2), locale::classic());
	    if( val == "id" )
		setDefaultMountBy( MOUNTBY_ID );
	    else if( val == "path" )
		setDefaultMountBy( MOUNTBY_PATH );
	    else if( val == "device" )
		setDefaultMountBy( MOUNTBY_DEVICE );
	    else if( val == "uuid" )
		setDefaultMountBy( MOUNTBY_UUID );
	    else if( val == "label" )
		setDefaultMountBy( MOUNTBY_LABEL );
	}
    }
    if( autodetect )
	{
	detectArch();
	efiboot = (arch() == "ia64");
	}
    if( testmode )
	{
	char * tenv = getenv( "YAST2_STORAGE_TDIR" );
	if( tenv!=NULL && strlen(tenv)>0 )
	    {
	    logdir = testdir = tenv;
	    }
	else
	    {
	    testdir = logdir;
	    }
	}
    y2mil("instsys:" << inst_sys << " testdir:" << testdir);
    detectObjects();
    setCacheChanges( true );
    dumpObjectList();

    std::list<std::pair<string,string> >::const_iterator i;
    for( i=infoPopupTxts.begin(); i!=infoPopupTxts.end(); ++i )
	{
	storage::usedBy ub;
	usedBy( i->first, ub );
	y2mil( "device:" << i->first << " used By:" << ub );
	if( ub.type()==UB_NONE )
	    infoPopupCb( i->second );
	else
	    y2mil( "suppressing cb:" << i->second );
	}
    logProcData();
    }

void Storage::dumpObjectList()
{
    ostringstream buf;
    classic(buf);
    printInfo(buf);
    std::list<string> l = splitString( buf.str(), "\n" );
    y2mil("DETECTED OBJECTS");
    for (std::list<string>::const_iterator i = l.begin(); i != l.end(); i++)
	y2mil(*i);
    y2mil("END DETECTED OBJECTS");
}

void Storage::detectObjects()
    {
    ProcPart* ppart = new ProcPart;
    detectDisks( *ppart );
    if( instsys() )
	{
	MdCo::activate( true, tmpDir() );
	LvmVg::activate( true );
	DmraidCo::activate( true );
	delete ppart;
	ppart = new ProcPart;
	}
    detectMds();
    detectDmraid( *ppart );
    detectDmmultipath( *ppart );
    detectLvmVgs();
    detectDm( *ppart );

    LvmVgPair p = lvgPair();
    y2mil( "p length:" << p.length() );
    for( LvmVgIterator i=p.begin(); i!=p.end(); ++i )
	i->normalizeDmDevices();

    if( testmode )
        {
	SystemCmd::testmode = true;
 	rootprefix = testdir;
 	fstab = new EtcFstab( rootprefix );
	string t = testdir+"/volume_info";
	if( access( t.c_str(), R_OK )==0 )
	    {
	    detectFsDataTestMode( t, vBegin(), vEnd() );
	    }
	}
    else
	{
	fstab = new EtcFstab( "/etc", isRootMounted() );
	detectLoops( *ppart );
	ProcMounts pm( this );
	if( !instsys() )
	    detectNfs( pm );
	detectFsData( vBegin(), vEnd(), pm );
	}

    if( instsys() )
	{
	SystemCmd c( "grep ^md.*dm- /proc/mdstat" );
	SystemCmd rm;
	for( unsigned i=0; i<c.numLines(); i++ )
	    {
	    rm.execute(MDADMBIN " --stop " + quote("/dev/" + extractNthWord(0, *c.getLine(i))));
	    }
	}
    delete ppart;
    }

void Storage::deleteClist( CCont& co )
    {
    for( CIter i=co.begin(); i!=co.end(); ++i )
	delete( *i );
    co.clear();
    }

void Storage::deleteBackups()
    {
    map<string,CCont>::iterator i = backups.begin();
    while( i!=backups.end() )
	{
	deleteClist( i->second );
	++i;
	}
    backups.clear();
    }

Storage::~Storage()
    {
    if( max_log_num>0 )
	{
	logVolumes( logdir );
	for( CIter i=cont.begin(); i!=cont.end(); ++i )
	    {
	    (*i)->logData( logdir );
	    }
	}
    deleteClist(cont);
    deleteBackups();
    if( !tempdir.empty() && access( tempdir.c_str(), R_OK )==0 )
	{
	SystemCmd c( "rmdir " + tempdir );
	if( c.retcode()!=0 )
	    {
	    y2err("stray tmpfile");
	    c.execute( "ls -l" + tempdir );
	    c.execute( "rm -rf " + tempdir );
	    }
	}
    y2mil("destructed Storage");
    }

void Storage::rescanEverything()
    {
    y2mil("rescan everything");
    deleteClist(cont);
    detectObjects();
    }

const string& Storage::tmpDir() const
    {
    if( access( tempdir.c_str(), W_OK )!= 0 )
	mkdir( tempdir.c_str(), 0700 );
    return( tempdir );
    }

void
Storage::detectArch()
    {
    struct utsname buf;
    proc_arch = "i386";
    if( uname( &buf ) == 0 )
	{
	if( strncmp( buf.machine, "ppc", 3 )==0 )
	    {
	    proc_arch = "ppc";
	    }
	else if( strncmp( buf.machine, "ia64", 4 )==0 )
	    {
	    proc_arch = "ia64";
	    }
	else if( strncmp( buf.machine, "s390", 4 )==0 )
	    {
	    proc_arch = "s390";
	    }
	else if( strncmp( buf.machine, "sparc", 5 )==0 )
	    {
	    proc_arch = "sparc";
	    }
	}
    if( proc_arch == "ppc" )
	{
	AsciiFile cpu( "/proc/cpuinfo" );
	int l = cpu.find( 0, "^machine\t" );
	y2mil("line:" << l);
	if( l >= 0 )
	    {
	    string line = cpu[l];
	    line = extractNthWord( 2, line );
	    y2mil("line:" << line);
	    is_ppc_mac = line.find( "PowerMac" )==0 || line.find( "PowerBook" )==0;
	    is_ppc_pegasos = line.find( "EFIKA5K2" )==0;
	    if( is_ppc_mac == 0 && is_ppc_pegasos == 0 )
		{
		line = cpu[l];
		line = extractNthWord( 3, line );
		y2mil("line:" << line);
		is_ppc_pegasos = line.find( "Pegasos" )==0;
		}
	    }
	}
    y2mil("Arch:" << proc_arch << " IsPPCMac:" << is_ppc_mac << " IsPPCPegasos:" << is_ppc_pegasos);
    }

void
Storage::detectDisks( ProcPart& ppart )
    {
    if( test() )
	{
	glob_t globbuf;

	if( glob( (testdir+"/disk_*[!~0-9]").c_str(), GLOB_NOSORT, 0,
	          &globbuf) == 0)
	    {
	    for (char** p = globbuf.gl_pathv; *p != 0; *p++)
		addToList( new Disk( this, *p ) );
	    }
 	globfree (&globbuf);
	}
    else if( autodetect )
	{
	autodetectDisks( ppart );
	}
    }


void Storage::detectMds()
    {
    if( test() )
	{
	string file = testdir+"/md";
	if( access( file.c_str(), R_OK )==0 )
	    {
	    addToList( new MdCo( this, file ) );
	    }
	}
    else
	{
	MdCo * v = new MdCo( this, true );
	if( !v->isEmpty() )
	    addToList( v );
	else
	    delete v;
	}
    }

void Storage::detectLoops( ProcPart& ppart )
    {
    if( test() )
	{
	string file = testdir+"/loop";
	if( access( file.c_str(), R_OK )==0 )
	    {
	    addToList( new LoopCo( this, file ) );
	    }
	}
    else
	{
	LoopCo * v = new LoopCo( this, true, ppart );
	if( !v->isEmpty() )
	    addToList( v );
	else
	    delete v;
	}
    }

void Storage::detectNfs( ProcMounts& mounts )
    {
    if( test() )
	{
	string file = testdir+"/nfs";
	if( access( file.c_str(), R_OK )==0 )
	    {
	    addToList( new NfsCo( this, file ) );
	    }
	}
    else
	{
	NfsCo * v = new NfsCo( this, mounts );
	if( !v->isEmpty() )
	    addToList( v );
	else
	    delete v;
	}
    }

void
Storage::detectLvmVgs()
    {
    if( test() )
	{
	glob_t globbuf;
	if( glob( (testdir+"/lvm_*[!~0-9]").c_str(), GLOB_NOSORT, 0,
	          &globbuf) == 0)
	    {
	    for (char** p = globbuf.gl_pathv; *p != 0; *p++)
		addToList( new LvmVg( this, *p, true ) );
	    }
 	globfree (&globbuf);
	}
    else if( getenv( "YAST2_STORAGE_NO_LVM" )==NULL )
	{
	list<string> l;
	LvmVg::getVgs( l );
	for( list<string>::const_iterator i=l.begin(); i!=l.end(); ++i )
	    {
	    LvmVg * v = new LvmVg( this, *i );
	    if( !v->inactive() )
		{
		addToList( v );
		v->checkConsistency();
		}
	    else
		{
		y2milestone( "inactive VG %s", i->c_str() );
		v->unuseDev();
		delete( v );
		}
	    }
	}
    }


void
Storage::detectDmraid( ProcPart& ppart )
    {
    if( test() )
	{
	glob_t globbuf;
	if( glob( (testdir+"/dmraid_*[!~0-9]").c_str(), GLOB_NOSORT, 0,
	          &globbuf) == 0)
	    {
	    // TODO: load test data
	    }
 	globfree (&globbuf);
	}
    else if( getenv( "YAST2_STORAGE_NO_DMRAID" )==NULL )
	{
	list<string> l;
	DmraidCo::getRaids( l );
	map<string, list<string>> by_id;
	if( !l.empty() )
	    getUdevMap("/dev/disk/by-id", by_id);
	for( list<string>::const_iterator i=l.begin(); i!=l.end(); ++i )
	    {
	    DmraidCo * v = new DmraidCo( this, *i, ppart );
	    if( v->isValid() )
		{
		list<string> nm = by_id["dm-"+decString(v->minorNr())];
		if( !nm.empty() )
		    v->setUdevData( nm );
		addToList( v );
		}
	    else
		{
		y2milestone( "inactive DMRAID %s", i->c_str() );
		v->unuseDev();
		delete( v );
		}
	    }
	}
    }


void
Storage::detectDmmultipath( ProcPart& ppart )
{
    if( test() )
    {
	glob_t globbuf;
	if( glob( (testdir+"/dmmultipath_*[!~0-9]").c_str(), GLOB_NOSORT, 0,
	          &globbuf) == 0)
	{
	    // TODO: load test data
	}
 	globfree (&globbuf);
    }
    else if( getenv( "YAST2_STORAGE_NO_DMMULTIPATH" )==NULL )
    {
	list<string> l;
	DmmultipathCo::getMultipaths( l );
	map<string, list<string>> by_id;
	if( !l.empty() )
	    getUdevMap("/dev/disk/by-id", by_id);
	for( list<string>::const_iterator i=l.begin(); i!=l.end(); ++i )
	{
	    DmmultipathCo * v = new DmmultipathCo( this, *i, ppart );
	    if( v->isValid() )
	    {
		list<string> nm = by_id["dm-"+decString(v->minorNr())];
		if( !nm.empty() )
		    v->setUdevData( nm );
		addToList( v );
	    }
	    else
	    {
		y2mil("inactive DMMULTIPATH " << *i);
		v->unuseDev();
		delete v;
	    }
	}
    }
}


void
Storage::detectDm( ProcPart& ppart )
    {
    if( test() )
	{
	glob_t globbuf;
	if( glob( (testdir+"/dm_*[!~0-9]").c_str(), GLOB_NOSORT, 0,
	          &globbuf) == 0)
	    {
	    // TODO: load test data
	    }
 	globfree (&globbuf);
	}
    else if( getenv( "YAST2_STORAGE_NO_DM" )==NULL )
	{
	DmCo * v = new DmCo( this, true, ppart );
	if( !v->isEmpty() )
	    {
	    addToList( v );
	    v->updateDmMaps();
	    }
	else
	    delete v;
	}
    }

namespace storage
{
struct DiskData
    {
    enum DTyp { DISK, DASD, XEN };

    DiskData() { d=0; s=0; typ=DISK; };
    DiskData( const string& n, DTyp t, unsigned long long sz ) { d=0; s=sz; typ=t; name=n; };

    Disk* d;
    DTyp typ;
    unsigned long long s;
    string name;
    string dev;
    };

std::ostream& operator<< ( std::ostream& s, const storage::DiskData& d )
    {
    s << d.name << "," << d.typ << "," << d.s << "," << d.d;
    if( !d.dev.empty() && d.dev!=d.name )
	s << "," << d.dev;
    return( s );
    }
}


void
Storage::initDisk( DiskData& data, ProcPart& pp )
    {
    y2mil( "data:" << data );
    data.dev = data.name;
    string::size_type pos = data.dev.find('!');
    while( pos!=string::npos )
	{
	data.dev[pos] = '/';
	pos = data.dev.find('!',pos+1);
	}
    y2milestone( "name sysfs:%s parted:%s", data.name.c_str(), 
                 data.dev.c_str() );
    Disk * d = NULL;
    switch( data.typ )
	{
	case DiskData::DISK:
	    d = new Disk( this, data.dev, data.s );
	    break;
	case DiskData::DASD:
	    d = new Dasd( this, data.dev, data.s );
	    break;
	case DiskData::XEN:
	    {
	    string::size_type p = data.dev.find_last_not_of( "0123456789" );
	    int nr = -1;
	    data.dev.substr( p+1 ) >> nr;
	    data.dev.erase( p+1 );
	    if( nr>=0 )
		{
		d = new Disk( this, data.dev, (unsigned)nr, data.s, pp );
		}
	    break;
	    }
	}
    if( d && 
        (d->getSysfsInfo(SYSFSDIR "/" + data.name)||data.typ==DiskData::XEN) &&
	(data.typ==DiskData::XEN||d->detect(pp)))
	{
	if( max_log_num>0 )
	    d->logData( logdir );
	data.d = d;
	}
    else if( d )
	{
	delete d;
	}
    }

void
Storage::autodetectDisks( ProcPart& ppart )
    {
    DIR *Dir;
    struct dirent *Entry;
    if( (Dir=opendir(SYSFSDIR))!=NULL )
    {
	map<string,list<string>> by_path;
	map<string,list<string>> by_id;
	getUdevMap("/dev/disk/by-path", by_path);
	getUdevMap("/dev/disk/by-id", by_id);
	list<DiskData> dl;
	while( (Entry=readdir( Dir ))!=NULL )
	    {
	    int Range=0;
	    unsigned long long Size = 0;
	    string SysfsDir = string(SYSFSDIR "/") + Entry->d_name;
	    string SysfsFile = SysfsDir + "/range";
	    y2mil("autodetectDisks sysfsdir:" << SysfsDir);
	    y2mil("autodetectDisks Range access:" << access(SysfsFile.c_str(), R_OK));
	    if( access( SysfsFile.c_str(), R_OK )==0 )
		{
		ifstream File( SysfsFile.c_str() );
		classic(File);
		File >> Range;
		}
	    SysfsFile = SysfsDir+"/size";
	    if( access( SysfsFile.c_str(), R_OK )==0 )
		{
		ifstream File( SysfsFile.c_str() );
		classic(File);
		File >> Size;
		}
	    string dn = Entry->d_name;
	    y2mil( "autodetectDisks Range:" << Range << " Size:" << Size );
	    if( Range>1 && (Size>0||dn.find( "dasd" )==0) )
		{
		DiskData::DTyp t = (dn.find( "dasd" )==0)?DiskData::DASD
		                                         :DiskData::DISK;
		dl.push_back( DiskData( dn, t, Size/2 ) );
		}
	    else if( Range==1 && Size>0 )
		{
		SysfsFile = SysfsDir+"/device";
		string devname;
		int ret;
		char lbuf[1024+1];
		if( access( SysfsFile.c_str(), R_OK )==0 &&
		    (ret=readlink( SysfsFile.c_str(), lbuf, sizeof(lbuf) ))>0 )
		    {
		    devname.append( lbuf, ret );
		    y2mil( "devname:" << devname );
		    }
		if( devname.find( "/xen/vbd" )!=string::npos &&
	            isdigit(dn[dn.size()-1]) )
		    {
		    dl.push_back( DiskData( dn, DiskData::XEN, Size/2 ) );
		    }
		}
	    }
	closedir( Dir );
	y2mil( "dl: " << dl );
	for( list<DiskData>::iterator i = dl.begin(); i!=dl.end(); ++i )
	    {
	    initDisk( *i, ppart );
	    }
	y2mil( "dl: " << dl );
	for( list<DiskData>::iterator i = dl.begin(); i!=dl.end(); ++i )
	    {
	    if( i->d )
	        {
		string tmp;
		if (!by_path[i->dev].empty())
		    tmp = by_path[i->dev].front();
		i->d->setUdevData(tmp, by_id[i->dev]);
		addToList( i->d );
		}
	    }
	}
    else
	{
	y2war("Failed to open:" SYSFSDIR);
	}
    }

void
Storage::detectFsData( const VolIterator& begin, const VolIterator& end,
                       ProcMounts& mounts )
    {
    y2milestone( "detectFsData begin" );
    SystemCmd Blkid( "BLKID_SKIP_CHECK_MDRAID=1 /sbin/blkid -c /dev/null" );
    SystemCmd Losetup(LOSETUPBIN " -a");
    for( VolIterator i=begin; i!=end; ++i )
	{
	if( i->getUsedByType()==UB_NONE )
	    {
	    i->getLoopData( Losetup );
	    i->getFsData( Blkid );
	    y2mil( "detect:" << *i );
	    }
	}
    for( VolIterator i=begin; i!=end; ++i )
	{
	if( i->getUsedByType()==UB_NONE )
	    {
	    i->getMountData( mounts, !detectMounted );
	    i->getFstabData( *fstab );
	    y2mil( "detect:" << *i );
	    if( i->getFs()==FSUNKNOWN && i->getEncryption()==ENC_NONE )
		i->getStartData();
	    }
	}
    if( max_log_num>0 )
	logVolumes( logdir );
    y2milestone( "detectFsData end" );
    }

void
Storage::printInfo( ostream& str, const string& name )
    {
    assertInit();
    ConstContPair p = contPair();
    string n = DmPartCo::undevName(name);
    for( ConstContIterator i=p.begin(); i!=p.end(); ++i )
	{
	if( name.empty() || i->name()==n )
	    {
	    Container::ConstVolPair vp = i->volPair();
	    i->print( str );
	    str << endl;
	    for( Container::ConstVolIterator j=vp.begin(); j!=vp.end(); ++j )
		{
		j->print( str );
		str << endl;
		}
	    }
	}
    }

void
Storage::detectFsDataTestMode( const string& file, const VolIterator& begin,
			       const VolIterator& end )
    {
    AsciiFile vd( file );
    for( VolIterator i=begin; i!=end; ++i )
	{
	int pos = -1;
	if( (pos=vd.find( 0, "^"+i->device()+" " ))>=0 )
	    {
	    i->getTestmodeData( vd[pos] );
	    }
	i->getFstabData( *fstab );
	}
    }

void
Storage::logVolumes( const string& Dir )
    {
    string fname( Dir + "/volume_info.tmp" );
    ofstream file( fname.c_str() );
    classic(file);
    for( VolIterator i=vBegin(); i!=vEnd(); ++i )
	{
	if( i->getFs()!=FSUNKNOWN )
	    {
	    i->logVolume( file );
	    }
	}
    file.close();
    handleLogFile( fname );
    }

bool
Storage::testFilesEqual( const string& n1, const string& n2 )
    {
    bool ret = false;
    if( access( n1.c_str(), R_OK )==0 && access( n2.c_str(), R_OK )==0 )
	{
	SystemCmd c("/usr/bin/md5sum " + quote(n1) + " " + quote(n2));
	if( c.retcode()==0 && c.numLines()>=2 )
	    {
	    ret = extractNthWord( 0, *c.getLine(0) )==
	          extractNthWord( 0, *c.getLine(1) );
	    }
	}
    y2mil("ret:" << ret << "n1:" << n1 << " n2:" << n2);
    return ret;
    }

void
Storage::handleLogFile( const string& name )
    {
    string bname( name );
    string::size_type pos = bname.rfind( '.' );
    bname.erase( pos );
    y2milestone( "name:%s bname:%s", name.c_str(), bname.c_str() );
    if( access( bname.c_str(), R_OK )==0 )
	{
	if( !testFilesEqual( bname, name ) )
	    {
	    unsigned num = max_log_num;
	    string tmpo = bname + "-" + decString(num);
	    string tmpn;
	    if( access( tmpo.c_str(), R_OK )==0 )
		unlink( tmpo.c_str() );
	    while( num>1 )
		{
		tmpn = bname + "-" + decString(num-1);
		if( access( tmpn.c_str(), R_OK )==0 )
		    rename( tmpn.c_str(), tmpo.c_str() );
		tmpo = tmpn;
		num--;
		}
	    rename( bname.c_str(), tmpn.c_str() );
	    rename( name.c_str(), bname.c_str() );
	    }
	else
	    unlink( name.c_str() );
	}
    else
	rename( name.c_str(), bname.c_str() );
    }

void Storage::setRecursiveRemoval( bool val )
    {
    y2milestone( "val:%d", val );
    recursiveRemove = val;
    }

void Storage::setZeroNewPartitions( bool val )
    {
    y2milestone( "val:%d", val );
    zeroNewPartitions = val;
    }

void Storage::setDefaultMountBy( MountByType val )
    {
    y2mil( "val:" << Volume::mbyTypeString(val) );
    defaultMountBy = val;
    }

void Storage::setEfiBoot(bool val)
{  
    y2milestone("val:%d", val);
    efiboot = val;
}

void Storage::setRootPrefix( const string& root )
    {
    y2milestone( "root:%s", root.c_str() );
    rootprefix = root;
    }

string Storage::prependRoot(const string& mp) const
{ 
    string ret = mp;
    if (mp != "swap")
	ret.insert(0, rootprefix);
    return ret;
}

void Storage::setDetectMountedVolumes( bool val )
    {
    y2milestone( "val:%d", val );
    detectMounted = val;
    }

string Storage::proc_arch;
bool Storage::is_ppc_mac = false;
bool Storage::is_ppc_pegasos = false;


namespace storage
{
    void initDefaultLogger ()
    {
	Storage::initDefaultLogger ();
    }

    StorageInterface* createDefaultStorageInterface ()
    {
	return new Storage ();
    }

    StorageInterface* createStorageInterface (bool ronly, bool testmode, bool autodetect)
    {
	return new Storage (ronly, testmode, autodetect);
    }

    StorageInterface* createStorageInterfacePid (bool ronly, bool testmode, bool autodetect,
						 pid_t& locker_pid)
    {
	try
	{
	    return new Storage (ronly, testmode, autodetect);
	}
	catch (const LockException& e)
	{
	    locker_pid = e.getLockerPid();
	    return NULL;
	}
    }

    void destroyStorageInterface (StorageInterface* p)
    {
	delete p;
    }
}


int
Storage::createPartition( const string& disk, PartitionType type, unsigned long start,
			  unsigned long size, string& device )
    {
    int ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s type:%d start:%ld size:%ld", disk.c_str(),
                 type, start, size );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 )
	{
	DiskIterator i = findDisk( disk );
	if( i != dEnd() )
	    {
	    done = true;
	    if( i->getUsedByType() != UB_NONE )
		ret = STORAGE_DISK_USED_BY;
	    else
		{
		ret = i->createPartition( type, start, size, device, true );
		}
	    }
	}
    if( ret==0 && !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    if( i->getUsedByType() != UB_NONE )
		ret = STORAGE_DISK_USED_BY;
	    else
		{
		ret = i->createPartition( type, start, size, device, true );
		}
	    }
	}  
    if( !done && ret==0 )
	{
	ret = STORAGE_DISK_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d device:%s", ret, ret?"":device.c_str() );
    return( ret );
    }

int
Storage::createPartitionKb( const string& disk, PartitionType type,
                            unsigned long long start,
			    unsigned long long sizeK, string& device )
    {
    int ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s type:%d start:%lld sizeK:%lld", disk.c_str(),
                 type, start, sizeK );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 )
	{
	DiskIterator i = findDisk( disk );
	if( i != dEnd() )
	    {
	    done = true;
	    if( i->getUsedByType() != UB_NONE )
		ret = STORAGE_DISK_USED_BY;
	    else
		{
		unsigned long num_cyl = i->kbToCylinder( sizeK );
		unsigned long long tmp_start = start;
		if( tmp_start > i->kbToCylinder(1)/2 )
		    tmp_start -= i->kbToCylinder(1)/2;
		else
		    tmp_start = 0;
		unsigned long start_cyl = i->kbToCylinder( tmp_start )+1;
		ret = i->createPartition( type, start_cyl, num_cyl, device, true );
		}
	    }
	}
    if( ret==0 && !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    if( i->getUsedByType() != UB_NONE )
		ret = STORAGE_DISK_USED_BY;
	    else
		{
		unsigned long num_cyl = i->kbToCylinder( sizeK );
		unsigned long long tmp_start = start;
		if( tmp_start > i->kbToCylinder(1)/2 )
		    tmp_start -= i->kbToCylinder(1)/2;
		else
		    tmp_start = 0;
		unsigned long start_cyl = i->kbToCylinder( tmp_start )+1;
		ret = i->createPartition( type, start_cyl, num_cyl, device, true );
		}
	    }
	}
    if( ret==0 && !done )
	{
	ret = STORAGE_DISK_NOT_FOUND;
	}
    y2milestone( "ret:%d device:%s", ret, ret?"":device.c_str() );
    return( ret );
    }

int
Storage::createPartitionAny( const string& disk, unsigned long long sizeK,
                             string& device )
    {
    int ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s sizeK:%lld", disk.c_str(), sizeK );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 )
	{
	DiskIterator i = findDisk( disk );
	if( i != dEnd() )
	    {
	    done = true;
	    if( i->getUsedByType() != UB_NONE )
		ret = STORAGE_DISK_USED_BY;
	    else
		{
		unsigned long num_cyl = i->kbToCylinder( sizeK );
		ret = i->createPartition( num_cyl, device, true );
		}
	    }
	}
    if( ret==0 && !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    if( i->getUsedByType() != UB_NONE )
		ret = STORAGE_DISK_USED_BY;
	    else
		{
		unsigned long num_cyl = i->kbToCylinder( sizeK );
		ret = i->createPartition( num_cyl, device, true );
		}
	    }
	}
    if( ret==0 && !done )
	{
	ret = STORAGE_DISK_NOT_FOUND;
	}
    y2milestone( "ret:%d device:%s", ret, ret?"":device.c_str() );
    return( ret );
    }

int
Storage::nextFreePartition( const string& disk, PartitionType type,
                            unsigned& nr, string& device )
    {
    int ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s type:%u", disk.c_str(), type );
    DiskIterator i = findDisk( disk );
    if( i != dEnd() )
	{
	done = true;
	ret = i->nextFreePartition( type, nr, device );
	}
    if( !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    ret = i->nextFreePartition( type, nr, device );
	    }
	}
    if( !done )
	{
	ret = STORAGE_DISK_NOT_FOUND;
	}
    y2milestone( "ret:%d device:%s", ret, ret?"":device.c_str() );
    return( ret );
    }

int
Storage::createPartitionMax( const string& disk, PartitionType type,
                             string& device )
    {
    int ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s type:%u", disk.c_str(), type );
    DiskIterator i = findDisk( disk );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 )
	{
	DiskIterator i = findDisk( disk );
	if( i != dEnd() )
	    {
	    done = true;
	    if( i->getUsedByType() != UB_NONE )
		ret = STORAGE_DISK_USED_BY;
	    else
		{
		ret = i->createPartition( type, device );
		}
	    }
	}
    if( ret==0 && !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    if( i->getUsedByType() != UB_NONE )
		ret = STORAGE_DISK_USED_BY;
	    else
		{
		ret = i->createPartition( type, device );
		}
	    }
	}
    if( ret==0 && !done )
	{
	ret = STORAGE_DISK_NOT_FOUND;
	}
    y2milestone( "ret:%d device:%s", ret, ret?"":device.c_str() );
    return( ret );
    }

unsigned long long
Storage::cylinderToKb( const string& disk, unsigned long size )
    {
    unsigned long long ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s size:%ld", disk.c_str(), size );
    DiskIterator i = findDisk( disk );
    if( i != dEnd() )
	{
	done = true;
	ret = i->cylinderToKb( size );
	}
    if( !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    ret = i->cylinderToKb( size );
	    }
	}
    y2milestone( "ret:%lld", ret );
    return( ret );
    }

unsigned long
Storage::kbToCylinder( const string& disk, unsigned long long sizeK )
    {
    unsigned long ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s sizeK:%lld", disk.c_str(), sizeK );
    DiskIterator i = findDisk( disk );
    if( i != dEnd() )
	{
	done = true;
	ret = i->kbToCylinder( sizeK );
	}
    if( !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    ret = i->kbToCylinder( sizeK );
	    }
	}
    y2milestone( "ret:%ld", ret );
    return( ret );
    }

int
Storage::removePartition( const string& partition )
    {
    int ret = 0;
    assertInit();
    y2milestone( "partition:%s", partition.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( partition, cont, vol ) )
	{
	if( cont->type()==DISK )
	    {
	    Disk* disk = dynamic_cast<Disk *>(&(*cont));
	    if( disk!=NULL )
		{
		if( vol->getUsedByType() == UB_NONE || recursiveRemove )
		    {
		    if( vol->getUsedByType() != UB_NONE )
			ret = removeUsing( vol->device(), vol->getUsedBy() );
		    if( ret==0 )
			ret = disk->removePartition( vol->nr() );
		    }
		else
		    ret = STORAGE_REMOVE_USED_VOLUME;
		}
	    else
		{
		ret = STORAGE_REMOVE_PARTITION_INVALID_CONTAINER;
		}
	    }
	else if( cont->type()==DMRAID || cont->type()==DMMULTIPATH )
	    {
	    DmPartCo* disk = dynamic_cast<DmPartCo *>(&(*cont));
	    if( disk!=NULL )
		{
		if( vol->getUsedByType() == UB_NONE || recursiveRemove )
		    {
		    if( vol->getUsedByType() != UB_NONE )
			ret = removeUsing( vol->device(), vol->getUsedBy() );
		    if( ret==0 )
			ret = disk->removePartition( vol->nr() );
		    }
		else
		    ret = STORAGE_REMOVE_USED_VOLUME;
		}
	    else
		{
		ret = STORAGE_REMOVE_PARTITION_INVALID_CONTAINER;
		}
	    }
	else
	    {
	    ret = STORAGE_REMOVE_PARTITION_INVALID_CONTAINER;
	    }
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::updatePartitionArea( const string& partition, unsigned long start,
                              unsigned long size )
    {
    int ret = 0;
    assertInit();
    y2milestone( "partition:%s start:%ld size:%ld", partition.c_str(),
                 start, size );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( partition, cont, vol ) )
	{
	if( cont->type()==DISK )
	    {
	    Disk* disk = dynamic_cast<Disk *>(&(*cont));
	    if( disk!=NULL )
		{
		ret = disk->changePartitionArea( vol->nr(), start, size );
		}
	    else
		{
		ret = STORAGE_CHANGE_AREA_INVALID_CONTAINER;
		}
	    }
	else if( cont->type()==DMRAID || cont->type()==DMMULTIPATH )
	    {
	    DmPartCo* disk = dynamic_cast<DmPartCo *>(&(*cont));
	    if( disk!=NULL )
		{
		ret = disk->changePartitionArea( vol->nr(), start, size );
		}
	    else
		{
		ret = STORAGE_CHANGE_AREA_INVALID_CONTAINER;
		}
	    }
	else
	    {
	    ret = STORAGE_CHANGE_AREA_INVALID_CONTAINER;
	    }
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changePartitionId( const string& partition, unsigned id )
    {
    int ret = 0;
    assertInit();
    y2milestone( "partition:%s id:%x", partition.c_str(), id );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( partition, cont, vol ) )
	{
	if( cont->type()==DISK )
	    {
	    Disk* disk = dynamic_cast<Disk *>(&(*cont));
	    if( disk!=NULL )
		{
		ret = disk->changePartitionId( vol->nr(), id );
		}
	    else
		{
		ret = STORAGE_CHANGE_PARTITION_ID_INVALID_CONTAINER;
		}
	    }
	else if( cont->type()==DMRAID || cont->type()==DMMULTIPATH )
	    {
	    DmPartCo* disk = dynamic_cast<DmPartCo *>(&(*cont));
	    if( disk!=NULL )
		{
		ret = disk->changePartitionId( vol->nr(), id );
		}
	    else
		{
		ret = STORAGE_CHANGE_PARTITION_ID_INVALID_CONTAINER;
		}
	    }
	else
	    {
	    ret = STORAGE_CHANGE_PARTITION_ID_INVALID_CONTAINER;
	    }
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::resizePartition( const string& partition, unsigned long sizeCyl )
    {
    return( resizePartition( partition, sizeCyl, false ));
    }

int
Storage::resizePartitionNoFs( const string& partition, unsigned long sizeCyl )
    {
    return( resizePartition( partition, sizeCyl, true ));
    }

int
Storage::resizePartition( const string& partition, unsigned long sizeCyl,
                          bool ignoreFs )
    {
    int ret = 0;
    assertInit();
    y2milestone( "partition:%s newCyl:%lu ignoreFs:%d", partition.c_str(), 
                 sizeCyl, ignoreFs );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( partition, cont, vol ) )
	{
	if( cont->type()==DISK )
	    {
	    Disk* disk = dynamic_cast<Disk *>(&(*cont));
	    Partition* p = dynamic_cast<Partition *>(&(*vol));
	    if( disk!=NULL && p!=NULL )
		{
		if( ignoreFs )
		    p->setIgnoreFs();
		ret = disk->resizePartition( p, sizeCyl );
		}
	    else
		{
		ret = STORAGE_RESIZE_INVALID_CONTAINER;
		}
	    }
	else if( cont->type()==DMRAID || cont->type()==DMMULTIPATH )
	    {
	    DmPartCo* disk = dynamic_cast<DmPartCo *>(&(*cont));
	    DmPart* p = dynamic_cast<DmPart *>(&(*vol));
	    if( disk!=NULL && p!=NULL )
		{
		if( ignoreFs )
		    p->setIgnoreFs();
		ret = disk->resizePartition( p, sizeCyl );
		}
	    else
		{
		ret = STORAGE_RESIZE_INVALID_CONTAINER;
		}
	    }
	else
	    {
	    ret = STORAGE_RESIZE_INVALID_CONTAINER;
	    }
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::forgetChangePartitionId( const string& partition )
    {
    int ret = 0;
    assertInit();
    y2milestone( "partition:%s", partition.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( partition, cont, vol ) )
	{
	if( cont->type()==DISK )
	    {
	    Disk* disk = dynamic_cast<Disk *>(&(*cont));
	    if( disk!=NULL )
		{
		ret = disk->forgetChangePartitionId( vol->nr() );
		}
	    else
		{
		ret = STORAGE_CHANGE_PARTITION_ID_INVALID_CONTAINER;
		}
	    }
	else if( cont->type()==DMRAID || cont->type()==DMMULTIPATH )
	    {
	    DmPartCo* disk = dynamic_cast<DmPartCo *>(&(*cont));
	    if( disk!=NULL )
		{
		ret = disk->forgetChangePartitionId( vol->nr() );
		}
	    else
		{
		ret = STORAGE_CHANGE_PARTITION_ID_INVALID_CONTAINER;
		}
	    }
	else
	    {
	    ret = STORAGE_CHANGE_PARTITION_ID_INVALID_CONTAINER;
	    }
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }


int
Storage::getUnusedPartitionSlots(const string& disk, list<PartitionSlotInfo>& slots)
{
    int ret = 0;
    slots.clear();
    assertInit();

    // TODO: don't have code below twice

    DiskIterator i1 = findDisk( disk );
    DmPartCoIterator i2 = findDmPartCo( disk );

    if (i1 != dEnd())
    {
	// maxPrimary() and maxLogical() include limits from partition table type and
	// minor number range

	bool primaryPossible = i1->numPrimary() + (i1->hasExtended() ? 1 : 0) < i1->maxPrimary();
	bool extendedPossible = primaryPossible && i1->extendedPossible() && !i1->hasExtended();
	bool logicalPossible = i1->hasExtended() && i1->numLogical() < (i1->maxLogical() - i1->maxPrimary());

	list<Region> regions;

	i1->getUnusedSpace(regions, false, false);
	for (list<Region>::const_iterator region=regions.begin(); region!=regions.end(); region++)
	{
	    PartitionSlotInfo slot;
	    slot.cylStart = region->start();
	    slot.cylSize = region->len();
	    slot.primarySlot = true;
	    slot.primaryPossible = primaryPossible;
	    slot.extendedSlot = true;
	    slot.extendedPossible = extendedPossible;
	    slot.logicalSlot = false;
	    slot.logicalPossible = false;
	    slots.push_back(slot);
	}

	i1->getUnusedSpace(regions, false, true);
	for (list<Region>::const_iterator region=regions.begin(); region!=regions.end(); region++)
	{
	    PartitionSlotInfo slot;
	    slot.cylStart = region->start();
	    slot.cylSize = region->len();
	    slot.primarySlot = false;
	    slot.primaryPossible = false;
	    slot.extendedSlot = false;
	    slot.extendedPossible = false;
	    slot.logicalSlot = true;
	    slot.logicalPossible = logicalPossible;
	    slots.push_back(slot);
	}
    }
    else if (i2 != dmpCoEnd())
    {
	// maxPrimary() and maxLogical() include limits from partition table type and
	// minor number range

	bool primaryPossible = i2->numPrimary() + (i2->hasExtended() ? 1 : 0) < i2->maxPrimary();
	bool extendedPossible = primaryPossible && i2->extendedPossible() && !i2->hasExtended();
	bool logicalPossible = i2->hasExtended() && i2->numLogical() < (i2->maxLogical() - i2->maxPrimary());

	list<Region> regions;

	i2->getUnusedSpace(regions, false, false);
	for (list<Region>::const_iterator region=regions.begin(); region!=regions.end(); region++)
	{
	    PartitionSlotInfo slot;
	    slot.cylStart = region->start();
	    slot.cylSize = region->len();
	    slot.primarySlot = true;
	    slot.primaryPossible = primaryPossible;
	    slot.extendedSlot = true;
	    slot.extendedPossible = extendedPossible;
	    slot.logicalSlot = false;
	    slot.logicalPossible = false;
	    slots.push_back(slot);
	}

	i2->getUnusedSpace(regions, false, true);
	for (list<Region>::const_iterator region=regions.begin(); region!=regions.end(); region++)
	{
	    PartitionSlotInfo slot;
	    slot.cylStart = region->start();
	    slot.cylSize = region->len();
	    slot.primarySlot = false;
	    slot.primaryPossible = false;
	    slot.extendedSlot = false;
	    slot.extendedPossible = false;
	    slot.logicalSlot = true;
	    slot.logicalPossible = logicalPossible;
	    slots.push_back(slot);
	}
    }
    else
    {
	ret = STORAGE_DISK_NOT_FOUND;
    }
    y2mil("ret:" << ret);
    return ret;
}


int
Storage::destroyPartitionTable( const string& disk, const string& label )
    {
    int ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s label:%s", disk.c_str(), label.c_str() );

    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 )
	{
	DiskIterator i = findDisk( disk );
	if( i != dEnd() )
	    {
	    done = true;
	    ret = i->destroyPartitionTable( label );
	    }
	}
    if( ret==0 && !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    ret = i->destroyPartitionTable( label );
	    }
	}
    if( ret==0 && !done )
	{
	ret = STORAGE_DISK_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::initializeDisk( const string& disk, bool value )
    {
    int ret = 0;
    bool done = false;
    assertInit();
    y2milestone( "disk:%s value:%d", disk.c_str(), value );

    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 )
	{
	DiskIterator i = findDisk( disk );
	if( i != dEnd() )
	    {
	    done = true;
	    ret = i->initializeDisk( value );
	    }
	}
    if( ret==0 && !done )
	{
	DmPartCoIterator i = findDmPartCo( disk );
	if( i != dmpCoEnd() )
	    {
	    done = true;
	    ret = DISK_INIT_NOT_POSSIBLE;
	    }
	}
    if( ret==0 && !done )
	{
	ret = STORAGE_DISK_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }


string
Storage::defaultDiskLabel() const
{
    return Disk::defaultLabel(this, 0);
}

string
Storage::defaultDiskLabelSize( unsigned long long size_k ) const
{
    return Disk::defaultLabel(this, size_k);
}


unsigned long long 
Storage::maxSizeLabelK( const string& label ) const
    {
    return( Disk::maxSizeLabelK(label) );
    }

int
Storage::changeFormatVolume( const string& device, bool format, FsType fs )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s format:%d type:%s", device.c_str(), format,
                 Volume::fsTypeString(fs).c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->setFormat( format, fs );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changeLabelVolume( const string& device, const string& label )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s label:%s", device.c_str(), label.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->setLabel( label );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::eraseLabelVolume( const string& device )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->eraseLabel();
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changeMkfsOptVolume( const string& device, const string& opts )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s opts:%s", device.c_str(), opts.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->setMkfsOption( opts );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changeTunefsOptVolume( const string& device, const string& opts )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s opts:%s", device.c_str(), opts.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->setTunefsOption( opts );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changeDescText( const string& device, const string& txt )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s txt:%s", device.c_str(), txt.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->setDescText( txt );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changeMountPoint( const string& device, const string& mount )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s mount:%s", device.c_str(), mount.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->changeMount( mount );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::getMountPoint( const string& device, string& mount )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str());
    VolIterator vol;
    ContIterator cont;
    if( findVolume( device, cont, vol ) )
	{
	mount = vol->getMount();
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changeMountBy( const string& device, MountByType mby )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s mby:%s", device.c_str(),
                 Volume::mbyTypeString(mby).c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->changeMountBy( mby );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::getMountBy( const string& device, MountByType& mby )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str());
    VolIterator vol;
    ContIterator cont;
    if( findVolume( device, cont, vol ) )
	{
	mby = vol->getMountBy();
	}
    else
	{
	mby = defaultMountBy;
	pair<string,unsigned> dp = Disk::getDiskPartition(device);
	y2mil( "dp:" << dp );
	DiskIterator i = findDisk( dp.first );
	if( (i==dEnd()) || (mby == MOUNTBY_ID && i->udevId().empty()) ||
	    (mby == MOUNTBY_PATH && i->udevPath().empty()))
	    mby = MOUNTBY_DEVICE;
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    y2milestone( "ret:%d mby:%s", ret, Volume::mbyTypeString(mby).c_str());
    return( ret );
    }

int
Storage::changeFstabOptions( const string& device, const string& options )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s options:%s", device.c_str(), options.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->changeFstabOptions( options );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::getFstabOptions( const string& device, string& options )
{
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str());
    VolIterator vol;
    ContIterator cont;
    if( findVolume( device, cont, vol ) )
    {
	options = vol->getFstabOption();
    }
    else
    {
	ret = STORAGE_VOLUME_NOT_FOUND;
    }
    y2milestone( "ret:%d options:%s", ret, options.c_str() );
    return( ret );
}

int
Storage::addFstabOptions( const string& device, const string& options )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s options:%s", device.c_str(), options.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	list<string> l = splitString( options, "," );
	list<string> opts = splitString( vol->getFstabOption(), "," );
	for( list<string>::const_iterator i=l.begin(); i!=l.end(); i++ )
	    {
	    if( find( opts.begin(), opts.end(), *i )==opts.end() )
		opts.push_back( *i );
	    }
	ret = vol->changeFstabOptions( mergeString( opts, "," ) );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::removeFstabOptions( const string& device, const string& options )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s options:%s", device.c_str(), options.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	list<string> l = splitString( options, "," );
	list<string> opts = splitString( vol->getFstabOption(), "," );
	for( list<string>::const_iterator i=l.begin(); i!=l.end(); i++ )
	    {
	    opts.remove_if(match_regex(*i));
	    }
	ret = vol->changeFstabOptions( mergeString( opts, "," ) );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::setCrypt( const string& device, bool val )
    {
    y2milestone( "device:%s val:%d", device.c_str(), val );
    return( setCryptType( device, val, ENC_LUKS ));
    }
    
int
Storage::setCryptType( const string& device, bool val, EncryptType typ )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s val:%d type:%d", device.c_str(), val, typ );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	ret = vol->setEncryption( val, typ );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::getCrypt( const string& device, bool& val )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str());
    VolIterator vol;
    ContIterator cont;
    if( findVolume( device, cont, vol ) )
	{
	val = vol->getEncryption();
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    y2milestone( "ret:%d val:%d", ret, val );
    return( ret );
    }

int
Storage::setCryptPassword( const string& device, const string& pwd )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s l:%zu", device.c_str(), pwd.length() );
#ifdef DEBUG_LOOP_CRYPT_PASSWORD
    y2milestone( "password:%s", pwd.c_str() );
#endif

    VolIterator vol;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, vol ) )
	{
	ret = vol->setCryptPwd( pwd );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::forgetCryptPassword( const string& device )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str() );

    VolIterator vol;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, vol ) )
	{
	vol->clearCryptPwd();
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::getCryptPassword( const string& device, string& pwd )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str() );

    pwd.clear();
    VolIterator vol;
    if( findVolume( device, vol ) )
	{
	pwd = vol->getCryptPwd();
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
#ifdef DEBUG_LOOP_CRYPT_PASSWORD
    y2milestone( "password:%s", pwd.c_str() );
#endif
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::setIgnoreFstab( const string& device, bool val )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s val:%d", device.c_str(), val );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	vol->setIgnoreFstab( val );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::getIgnoreFstab( const string& device, bool& val )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str());
    VolIterator vol;
    ContIterator cont;
    if( findVolume( device, cont, vol ) )
	{
	val = vol->ignoreFstab();
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    y2milestone( "ret:%d val:%d", ret, val );
    return( ret );
    }

int
Storage::resizeVolume( const string& device, unsigned long long newSizeMb )
    {
    return( resizeVolume( device, newSizeMb, false ));
    }

int
Storage::resizeVolumeNoFs( const string& device, unsigned long long newSizeMb )
    {
    return( resizeVolume( device, newSizeMb, true ));
    }

int
Storage::resizeVolume( const string& device, unsigned long long newSizeMb,
                       bool ignoreFs )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s newSizeMb:%llu ignoreFs:%d", device.c_str(), 
                 newSizeMb, ignoreFs );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	y2mil( "vol:" << *vol );
	if( ignoreFs )
	    vol->setIgnoreFs();
	ret = cont->resizeVolume( &(*vol), newSizeMb*1024 );
	eraseFreeInfo( vol->device() );
	y2mil( "vol:" << *vol );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::forgetResizeVolume( const string& device )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str() );
    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	vol->forgetResize();
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::removeVolume( const string& device )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s", device.c_str() );

    VolIterator vol;
    ContIterator cont;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( findVolume( device, cont, vol ) )
	{
	if( vol->getUsedByType() == UB_NONE || recursiveRemove )
	    {
	    string vdev = vol->device();
	    if( vol->getUsedByType() != UB_NONE )
		ret = removeUsing( vdev, vol->getUsedBy() );
	    if( ret==0 )
		ret = cont->removeVolume( &(*vol) );
	    }
	else
	    ret = STORAGE_REMOVE_USED_VOLUME;
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::createLvmVg( const string& name, unsigned long long peSizeK,
		      bool lvm1, const deque<string>& devs )
    {
    int ret = 0;
    assertInit();
    y2mil( "name:" << name << " peSizeK:" << peSizeK << " lvm1:" << lvm1 <<
	   " devices:" << devs );
    LvmVgIterator i = findLvmVg( name );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( name.empty() ||
        name.find_first_of( "\"\' /\n\t:*?" ) != string::npos )
	{
	ret = STORAGE_VG_INVALID_NAME;
	}
    else if( i == lvgEnd() )
	{
	LvmVg *v = new LvmVg( this, name, lvm1 );
	v->setCreated();
	ret = v->setPeSize( peSizeK );
	if( ret==0 && !devs.empty() )
	    {
	    list<string> d;
	    back_insert_iterator<list<string> > inserter(d);
	    copy( devs.begin(), devs.end(), inserter );
	    ret = v->extendVg( d );
	    }
	if( ret==0 )
	    addToList( v );
	else
	    delete( v );
	}
    else
	{
	ret = STORAGE_LVM_VG_EXISTS;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::removeLvmVg( const string& name )
    {
    int ret = 0;
    assertInit();
    y2milestone( "name:%s", name.c_str() );
    LvmVgIterator i = findLvmVg( name );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( i != lvgEnd() )
	{
	ret = i->removeVg();
	if( ret==0 && i->created() )
	    ret = removeContainer( &(*i) );
	}
    else
	{
	ret = STORAGE_LVM_VG_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::extendLvmVg( const string& name, const deque<string>& devs )
    {
    int ret = 0;
    assertInit();
    y2mil( "name:" << name << " devices:" << devs );
    LvmVgIterator i = findLvmVg( name );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( i != lvgEnd() )
	{
	list<string> d;
	back_insert_iterator<list<string> > inserter(d);
	copy( devs.begin(), devs.end(), inserter );
	ret = i->extendVg( d );
	}
    else
	{
	ret = STORAGE_LVM_VG_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::shrinkLvmVg( const string& name, const deque<string>& devs )
    {
    int ret = 0;
    assertInit();
    y2mil( "name:" << name << " devices:" << devs );
    LvmVgIterator i = findLvmVg( name );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( i != lvgEnd() )
	{
	list<string> d;
	back_insert_iterator<list<string> > inserter(d);
	copy( devs.begin(), devs.end(), inserter );
	ret = i->reduceVg( d );
	}
    else
	{
	ret = STORAGE_LVM_VG_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::createLvmLv( const string& vg, const string& name,
                      unsigned long long sizeM, unsigned stripe,
		      string& device )
    {
    int ret = 0;
    assertInit();
    y2milestone( "vg:%s name:%s sizeM:%llu stripe:%u", vg.c_str(),
                 name.c_str(), sizeM, stripe );
    LvmVgIterator i = findLvmVg( vg );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( i != lvgEnd() )
	{
	ret = i->createLv( name, sizeM*1024, stripe, device );
	}
    else
	{
	ret = STORAGE_LVM_VG_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d device:%s", ret, ret?"":device.c_str() );
    return( ret );
    }

int
Storage::removeLvmLvByDevice( const string& device )
    {
    int ret = 0;
    string vg, name;
    string d = undevDevice( device );
    string::size_type pos = d.find( '/' );
    if( pos!=string::npos )
	{
	vg = d.substr( 0, pos );
	name = d.substr( pos+1 );
	}
    if( !vg.empty() && !name.empty() )
	ret = removeLvmLv( vg, name );
    else
	ret = STORAGE_LVM_INVALID_DEVICE;
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::removeLvmLv( const string& vg, const string& name )
    {
    int ret = 0;
    assertInit();
    y2milestone( "vg:%s name:%s", vg.c_str(), name.c_str() );
    LvmVgIterator i = findLvmVg( vg );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( i != lvgEnd() )
	{
	ret = i->removeLv( name );
	}
    else
	{
	ret = STORAGE_LVM_VG_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changeLvStripeCount( const string& vg, const string& name,
			      unsigned long stripe )
    {
    int ret = 0;
    assertInit();
    y2milestone( "vg:%s name:%s stripe:%lu", vg.c_str(), name.c_str(),
                 stripe );
    LvmVgIterator i = findLvmVg( vg );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( i != lvgEnd() )
	{
	ret = i->changeStripe( name, stripe );
	}
    else
	{
	ret = STORAGE_LVM_VG_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::changeLvStripeSize( const string& vg, const string& name,
			     unsigned long long stripeSize )
    {
    int ret = 0;
    assertInit();
    y2milestone( "vg:%s name:%s stripeSize:%llu", vg.c_str(), name.c_str(),
                 stripeSize );
    LvmVgIterator i = findLvmVg( vg );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    else if( i != lvgEnd() )
	{
	ret = i->changeStripeSize( name, stripeSize );
	}
    else
	{
	ret = STORAGE_LVM_VG_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }


int
Storage::createLvmLvSnapshot(const string& vg, const string& origin,
			     const string& name, unsigned long long cowSizeK,
			     string& device)
{
    int ret = 0;
    device.erase();
    assertInit();
    y2mil("vg:" << vg << " origin:" << origin << " name:" << name << " cowSizeK:" << cowSizeK);
    LvmVgIterator i = findLvmVg(vg);
    if (readonly)
    {
	ret = STORAGE_CHANGE_READONLY;
    }
    else if (i != lvgEnd())
    {
	ret = i->createLvSnapshot(origin, name, cowSizeK, device);
    }
    else
    {
	ret = STORAGE_LVM_VG_NOT_FOUND;
    }
    if (ret == 0)
    {
	ret = checkCache();
    }
    y2mil("ret:" << ret << " device:" << device);
    return ret;
}


int
Storage::removeLvmLvSnapshot(const string& vg, const string& name)
{
    int ret = 0;
    assertInit();
    y2mil("vg:" << vg << " name:" << name);
    LvmVgIterator i = findLvmVg(vg);
    if (readonly)
    {
	ret = STORAGE_CHANGE_READONLY;
    }
    else if (i != lvgEnd())
    {
	ret = i->removeLvSnapshot(name);
    }
    else
    {
	ret = STORAGE_LVM_VG_NOT_FOUND;
    }
    if (ret == 0)
    {
	ret = checkCache();
    }
    y2mil("ret:" << ret);
    return ret;
}


int
Storage::getLvmLvSnapshotStateInfo(const string& vg, const string& name, 
				   LvmLvSnapshotStateInfo& info)
{
    int ret = 0;
    assertInit();
    y2mil("vg:" << vg << " name:" << name);
    LvmVgIterator i = findLvmVg(vg);
    if (i != lvgEnd())
    {
	ret = i->getLvSnapshotState(name, info);
    }
    else
    {
	ret = STORAGE_LVM_VG_NOT_FOUND;
    }
    y2mil("ret:" << ret);
    return ret;
}


int
Storage::nextFreeMd(int &nr, string &device)
{
    int ret = 0;
    assertInit();
    MdCo *md = NULL;
    nr = 0;
    if (haveMd(md))
	nr = md->unusedNumber();
    device = "/dev/md" + decString(nr);
    y2milestone("ret:%d nr:%d device:%s", ret, nr, device.c_str());
    return ret;
}

int
Storage::createMd( const string& name, MdType rtype,
                   const deque<string>& devs )
    {
    int ret = 0;
    assertInit();
    y2mil( "name:" << name << " MdType:" << Md::pName(rtype) <<
           " devices:" << devs );
    unsigned num = 0;
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 && !Md::mdStringNum( name, num ))
	{
	ret = STORAGE_MD_INVALID_NAME;
	}
    MdCo *md = NULL;
    bool have_md = true;
    if( ret==0 )
	{
	have_md = haveMd(md);
	if( !have_md )
	    md = new MdCo( this, false );
	}
    if( ret==0 && md!=NULL )
	{
	list<string> d;
	d.insert( d.end(), devs.begin(), devs.end() );
	ret = md->createMd( num, rtype, d );
	}
    if( !have_md )
	{
	if( ret==0 )
	    addToList( md );
	else if( md!=NULL )
	    delete md;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int Storage::createMdAny( MdType rtype, const deque<string>& devs,
			  string& device )
    {
    int ret = 0;
    assertInit();
    y2mil( "MdType:" << Md::pName(rtype) << " devices:" << devs );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    MdCo *md = NULL;
    bool have_md = true;
    unsigned num = 0;
    if( ret==0 )
	{
	have_md = haveMd(md);
	if( !have_md )
	    md = new MdCo( this, false );
	else
	    num = md->unusedNumber();
	if( md==NULL )
	    ret = STORAGE_MEMORY_EXHAUSTED;
	}
    if( ret==0 )
	{
	list<string> d;
	d.insert( d.end(), devs.begin(), devs.end() );
	ret = md->createMd( num, rtype, d );
	}
    if( !have_md )
	{
	if( ret==0 )
	    {
	    addToList( md );
	    }
	else if( md!=NULL )
	    delete md;
	}
    if( ret==0 )
	{
	device = "/dev/md" + decString(num);
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d device:%s", ret, ret==0?device.c_str():"" );
    return( ret );
    }

int Storage::removeMd( const string& name, bool destroySb )
    {
    int ret = 0;
    assertInit();
    y2milestone( "name:%s destroySb:%d", name.c_str(), destroySb );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    unsigned num = 0;
    if( ret==0 && !Md::mdStringNum( name, num ))
	{
	ret = STORAGE_MD_INVALID_NAME;
	}
    if( ret==0 )
	{
	MdCo *md = NULL;
	if( haveMd(md) )
	    ret = md->removeMd( num, destroySb );
	else
	    ret = STORAGE_MD_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int Storage::extendMd( const string& name, const string& dev )
    {
    int ret = 0;
    assertInit();
    y2milestone( "name:%s dev:%s", name.c_str(), dev.c_str() );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    unsigned num = 0;
    if( ret==0 && !Md::mdStringNum( name, num ))
	{
	ret = STORAGE_MD_INVALID_NAME;
	}
    if( ret==0 )
	{
	MdCo *md = NULL;
	if( haveMd(md) )
	    ret = md->extendMd( num, dev );
	else
	    ret = STORAGE_MD_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int Storage::shrinkMd( const string& name, const string& dev )
    {
    int ret = 0;
    assertInit();
    y2milestone( "name:%s dev:%s", name.c_str(), dev.c_str() );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    unsigned num = 0;
    if( ret==0 && !Md::mdStringNum( name, num ))
	{
	ret = STORAGE_MD_INVALID_NAME;
	}
    if( ret==0 )
	{
	MdCo *md = NULL;
	if( haveMd(md) )
	    ret = md->shrinkMd( num, dev );
	else
	    ret = STORAGE_MD_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int Storage::changeMdType( const string& name, MdType rtype )
    {
    int ret = 0;
    assertInit();
    y2milestone( "name:%s rtype:%d", name.c_str(), rtype );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    unsigned num = 0;
    if( ret==0 && !Md::mdStringNum( name, num ))
	{
	ret = STORAGE_MD_INVALID_NAME;
	}
    if( ret==0 )
	{
	MdCo *md = NULL;
	if( haveMd(md) )
	    ret = md->changeMdType( num, rtype );
	else
	    ret = STORAGE_MD_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int Storage::changeMdChunk( const string& name, unsigned long chunk )
    {
    int ret = 0;
    assertInit();
    y2milestone( "name:%s dev:%lu", name.c_str(), chunk );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    unsigned num = 0;
    if( ret==0 && !Md::mdStringNum( name, num ))
	{
	ret = STORAGE_MD_INVALID_NAME;
	}
    if( ret==0 )
	{
	MdCo *md = NULL;
	if( haveMd(md) )
	    ret = md->changeMdChunk( num, chunk );
	else
	    ret = STORAGE_MD_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int Storage::changeMdParity( const string& name, MdParity ptype )
    {
    int ret = 0;
    assertInit();
    y2milestone( "name:%s parity:%d", name.c_str(), ptype );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    unsigned num = 0;
    if( ret==0 && !Md::mdStringNum( name, num ))
	{
	ret = STORAGE_MD_INVALID_NAME;
	}
    if( ret==0 )
	{
	MdCo *md = NULL;
	if( haveMd(md) )
	    ret = md->changeMdParity( num, ptype );
	else
	    ret = STORAGE_MD_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int Storage::checkMd( const string& name )
    {
    int ret = 0;
    assertInit();
    y2milestone( "name:%s", name.c_str() );
    unsigned num = 0;
    MdCo *md = NULL;
    if( Md::mdStringNum( name, num ) && haveMd(md) )
	ret = md->checkMd(num);
    else
	ret = STORAGE_MD_NOT_FOUND;
    y2milestone( "ret:%d", ret );
    return( ret );
    }


int Storage::getMdStateInfo(const string& name, MdStateInfo& info)
{
    int ret = 0;
    assertInit();
    y2milestone("name:%s", name.c_str());
    unsigned num = 0;
    if (ret == 0 && !Md::mdStringNum(name, num))
    {
	ret = STORAGE_MD_INVALID_NAME;
    }
    if (ret == 0)
    {
	MdCo *md = NULL;
	if (haveMd(md))
	    ret = md->getMdState(num, info);
	else
	    ret = STORAGE_MD_NOT_FOUND;
    }
    y2milestone("ret:%d", ret);
    return ret;
}


int
Storage::computeMdSize(MdType md_type, list<string> devices, unsigned long long& sizeK)
{
    int ret = 0;

    unsigned long long sumK = 0;
    unsigned long long smallestK = 0;

    for (list<string>::const_iterator i = devices.begin(); i != devices.end(); i++)
    {
	const Volume* v = getVolume(*i);
	if (!v)
	{
	    ret = STORAGE_VOLUME_NOT_FOUND;
	    break;
	}

	sumK += v->sizeK();
	if (smallestK == 0)
	    smallestK = v->sizeK();
	else
	    smallestK = min(smallestK, v->sizeK());
    }

    switch (md_type)
    {
	case RAID0:
	    sizeK = sumK;
	    break;
	case RAID1:
	case MULTIPATH:
	    sizeK = smallestK;
	    break;
	case RAID5:
	    sizeK = devices.size()<1 ? 0 : smallestK*(devices.size()-1);
	    break;
	case RAID6:
	    sizeK = devices.size()<2 ? 0 : smallestK*(devices.size()-2);
	    break;
	case RAID10:
	    sizeK = smallestK*devices.size()/2;
	    break;
	default:
	    break;
    }

    y2milestone ("type:%d smallest:%llu sum:%llu size:%llu", md_type,
		 smallestK, sumK, sizeK);

    return ret;
}


bool Storage::haveMd( MdCo*& md )
    {
    md = NULL;
    CPair p = cPair();
    ContIterator i = p.begin();
    while( i != p.end() && i->type()!=MD )
	++i;
    if( i != p.end() )
	md = static_cast<MdCo*>(&(*i));
    return( i != p.end() );
    }

bool Storage::haveNfs( NfsCo*& co )
    {
    co = NULL;
    CPair p = cPair();
    ContIterator i = p.begin();
    while( i != p.end() && i->type()!=NFSC )
	++i;
    if( i != p.end() )
	co = static_cast<NfsCo*>(&(*i));
    return( i != p.end() );
    }

int 
Storage::addNfsDevice( const string& nfsDev, const string& opts,
                       unsigned long long sizeK, const string& mp )
    {
    int ret = 0;
    assertInit();
    y2mil( "name:" << nfsDev << " sizeK:" << sizeK << " mp:" << mp );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    NfsCo *co = NULL;
    bool have = true;
    if( ret==0 )
	{
	have = haveNfs(co);
	if( !have )
	    co = new NfsCo( this );
	}
    if( ret==0 && co!=NULL )
	{
	if( sizeK==0 )
	    checkNfsDevice( nfsDev, opts, sizeK );
	ret = co->addNfs( nfsDev, sizeK, mp );
	}
    if( !have )
	{
	if( ret==0 )
	    addToList( co );
	else if( co!=NULL )
	    delete co;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int 
Storage::checkNfsDevice( const string& nfsDev, const string& opts,
                         unsigned long long& sizeK )
    {
    int ret = 0;
    assertInit();
    NfsCo co( this );
    string mdir = tmpDir() + "/tmp_mp";
    unlink( mdir.c_str() );
    rmdir( mdir.c_str() );
    createPath( mdir );
    ret = co.addNfs( nfsDev, 0, "" );
    if( !opts.empty() )
	co.vBegin()->setFstabOption( opts );
    if( instsys() )
	{
	SystemCmd c;
	string prog_name = "/sbin/rpcbind";

	//We don't have rpcbind (#423026, #427428) ...
	if ( !checkNormalFile(prog_name) )
	{
	    //... so let's try portmap instead
	    y2mil("No rpcbind found, trying portmap instead ...");
	    prog_name = "/sbin/portmap";
	}

	c.execute( prog_name );
	c.execute( "/usr/sbin/rpc.statd" );
	}
    if( ret==0 && (ret=co.vBegin()->mount( mdir ))==0 )
	{
	sizeK = getDfSize( mdir );
	ret = co.vBegin()->umount( mdir );
	}
    y2mil( "name:" << nfsDev << " opts:" << opts << " ret:" << ret <<
           " sizeK:" << sizeK );
    return( ret );
    }

int
Storage::createFileLoop( const string& lname, bool reuseExisting,
                         unsigned long long sizeK, const string& mp,
			 const string& pwd, string& device )
    {
    int ret = 0;
    assertInit();
    y2milestone( "lname:%s reuseExisting:%d sizeK:%llu mp:%s", lname.c_str(),
                 reuseExisting, sizeK, mp.c_str() );
#ifdef DEBUG_LOOP_CRYPT_PASSWORD
    y2milestone( "pwd:%s", pwd.c_str() );
#endif
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    LoopCo *loop = NULL;
    Volume *vol = NULL;
    bool have_loop = true;
    if( ret==0 )
	{
	have_loop = haveLoop(loop);
	y2mil( "have_loop:" << have_loop );
	if( !have_loop )
	    {
	    ProcPart ppart;
	    loop = new LoopCo( this, false, ppart );
	    }
	if( loop==NULL )
	    ret = STORAGE_MEMORY_EXHAUSTED;
	}
    if( ret==0 )
	{
	ret = loop->createLoop( lname, reuseExisting, sizeK, true, device );
	}
    if( ret==0 && !loop->findVolume( device, vol ))
	{
	ret = STORAGE_CREATED_LOOP_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = vol->setCryptPwd( pwd );
	}
    if( ret==0 && (!reuseExisting || access( (root()+lname).c_str(), R_OK )!=0 ))
	{
	ret = vol->setFormat( true, EXT3 );
	}
    if( ret==0 )
	{
	ret = vol->setEncryption( true );
	}
    if( ret==0 and !mp.empty() )
	{
	ret = vol->changeMount( mp );
	}
    if( !have_loop )
	{
	if( ret==0 )
	    {
	    addToList( loop );
	    }
	else if( loop!=NULL )
	    delete loop;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d device:%s", ret, ret==0?device.c_str():"" );
    return( ret );
    }

int
Storage::modifyFileLoop( const string& device, const string& lname, 
                         bool reuseExisting, unsigned long long sizeK )
    {
    int ret = 0;
    assertInit();
    y2milestone( "device:%s lname:%s reuse:%d sizeK:%lld", device.c_str(), 
                 lname.c_str(), reuseExisting, sizeK );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 )
	{
	LoopCo *loop = NULL;
	if( haveLoop(loop) )
	    ret = loop->updateLoop( device, lname, reuseExisting, sizeK );
	else
	    ret = STORAGE_LOOP_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::removeFileLoop( const string& lname, bool removeFile )
    {
    int ret = 0;
    assertInit();
    y2milestone( "lname:%s removeFile:%d", lname.c_str(), removeFile );
    if( readonly )
	{
	ret = STORAGE_CHANGE_READONLY;
	}
    if( ret==0 )
	{
	LoopCo *loop = NULL;
	if( haveLoop(loop) )
	    ret = loop->removeLoop( lname, removeFile );
	else
	    ret = STORAGE_LOOP_NOT_FOUND;
	}
    if( ret==0 )
	{
	ret = checkCache();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }


bool Storage::haveLoop( LoopCo*& loop )
    {
    loop = NULL;
    CPair p = cPair();
    ContIterator i = p.begin();
    while( i != p.end() && i->type()!=LOOP )
	++i;
    if( i != p.end() )
	loop = static_cast<LoopCo*>(&(*i));
    return( i != p.end() );
    }

int Storage::removeDmraid( const string& name )
    {
    int ret = 0;
    assertInit();
    DmraidCoIterator i = findDmraidCo( name );
    if( i != dmrCoEnd() )
	{
	ret = i->removeDmPart();
	}
    else
	ret = STORAGE_DMRAID_CO_NOT_FOUND;
    return( ret );
    }

int Storage::checkCache()
    {
    int ret=0;
    if( !isCacheChanges() )
	ret = commit();
    return(ret);
    }


deque<string> 
Storage::getCommitActions(bool mark_destructive) const
{
    CommitInfo info;
    getCommitInfo(mark_destructive, info);
    return info.actions;
}


void
Storage::getCommitInfo(bool mark_destructive, CommitInfo& info) const
{
    info.destructive = false;
    info.actions.clear();
    
    ConstContPair p = contPair();
    y2mil("empty:" << p.empty());
    if( !p.empty() )
    {
	list<commitAction*> ac;
	for( ConstContIterator i = p.begin(); i != p.end(); ++i )
	{
	    list<commitAction*> l;
	    i->getCommitActions( l );
	    ac.splice( ac.end(), l );
	}
	ac.sort( cont_less<commitAction>() );
	string txt;
	for( list<commitAction*>::const_iterator i=ac.begin(); i!=ac.end(); ++i )
	{
	    if ((*i)->destructive)
		info.destructive = true;
	    txt.erase();
	    if( mark_destructive && (*i)->destructive )
		txt += "<font color=red>";
	    txt += (*i)->descr;
	    const Volume *v = (*i)->vol();
	    if( v && !v->getDescText().empty() )
	    {
		txt += ". ";
		txt += v->getDescText();
	    }
	    if( mark_destructive && (*i)->destructive )
		txt += "</font>";
	    info.actions.push_back( txt );
	    delete *i;
	}
    }
    y2mil("destructive:" << info.destructive << " actions.size():" << info.actions.size());
}


static bool sort_cont_up( const Container* rhs, const Container* lhs )
    {
    return( *rhs > *lhs );
    }

static bool sort_cont_down( const Container* rhs, const Container* lhs )
    {
    return( *rhs < *lhs );
    }

static bool sort_vol_normal( const Volume* rhs, const Volume* lhs )
    {
    return( *rhs < *lhs );
    }

static bool sort_vol_delete( const Volume* rhs, const Volume* lhs )
    {
    if( rhs->isMounted()==lhs->isMounted()  )
	{
        if( rhs->isMounted() )
	    return( rhs->getMount()>lhs->getMount() );
	else
	    {
	    if( rhs->needShrink() == lhs->needShrink() )
		{
		if( rhs->cType()!=DISK || rhs->cType()!=DISK )
		    return( *rhs > *lhs );
		else
		    {
		    return( *static_cast<const Partition*>(rhs) >
		            *static_cast<const Partition*>(lhs) );
		    }
		}
	    else
		return( rhs->needShrink() );
	    }
	}
    else
	return( rhs->isMounted() );
    }

static bool sort_vol_create( const Volume* rhs, const Volume* lhs )
    {
    if( rhs->cType()==lhs->cType() )
	{
	if( rhs->cType()==LVM )
	    return( static_cast<const Dm*>(rhs)->stripes() >
	            static_cast<const Dm*>(lhs)->stripes() );
	else
	    return( *rhs < *lhs );
	}
    else
	return( *rhs < *lhs );
    }

static bool sort_vol_mount( const Volume* rhs, const Volume* lhs )
    {
    if( rhs->getMount()=="swap" )
	return( false );
    else if( lhs->getMount()=="swap" )
	return( true );
    else if( lhs->hasOrigMount() != rhs->hasOrigMount() )
	return( rhs->hasOrigMount() );
    else
	return( rhs->getMount()<lhs->getMount() );
    }

void
Storage::sortCommitLists( CommitStage stage, list<Container*>& co,
                          list<Volume*>& vl, list<commitAction*>& todo )
    {
    co.sort( (stage==DECREASE)?sort_cont_up:sort_cont_down );
    std::ostringstream b;
    classic(b);
    if( stage==DECREASE )
	{
	vl.reverse();
	vl.sort( sort_vol_delete );
	}
    else if( stage==INCREASE )
	vl.sort( sort_vol_create );
    else if( stage==MOUNT )
	vl.sort( sort_vol_mount );
    else
	vl.sort( sort_vol_normal );
    for( list<Container*>::const_iterator i=co.begin(); i!=co.end(); ++i )
	todo.push_back( new commitAction( stage, (*i)->type(), *i ));
    for( list<Volume*>::const_iterator i=vl.begin(); i!=vl.end(); ++i )
	todo.push_back( new commitAction( stage, (*i)->cType(), *i ));
    b.str("");
    b << "unsorted actions <";
    for( list<commitAction*>::const_iterator i=todo.begin(); i!=todo.end(); ++i )
	{
	if( i!=todo.begin() )
	    b << " ";
	if( (*i)->container )
	    b << "C:" << (*i)->co()->device();
	else
	    b << "V:" << (*i)->vol()->device();
	}
    b << "> ";
    y2mil(b.str());
    b.str("");
    todo.sort( cont_less<commitAction>() );
    y2milestone( "stage %d", stage );
    b << "sorted co <";
    for( list<Container*>::const_iterator i=co.begin(); i!=co.end(); ++i )
	{
	if( i!=co.begin() )
	    b << " ";
	b << (*i)->name();
	}
    b << "> ";
    y2mil(b.str());
    b.str("");
    b << "sorted vol <";
    for( list<Volume*>::const_iterator i=vl.begin(); i!=vl.end(); ++i )
	{
	if( i!=vl.begin() )
	    b << " ";
	b << (*i)->device();
	}
    b << "> ";
    y2mil(b.str());
    b.str("");
    b << "sorted actions <";
    for( list<commitAction*>::const_iterator i=todo.begin(); i!=todo.end(); ++i )
	{
	if( i!=todo.begin() )
	    b << " ";
	if( (*i)->container )
	    b << "C:" << (*i)->co()->device();
	else
	    b << "V:" << (*i)->vol()->device();
	}
    b << "> ";
    y2mil(b.str());
    }

void Storage::handleHald( bool stop )
    {
    y2mil( "stop:" << stop );
    int ret;
    if( stop )
	{
	hald_pid = 0;
	SystemCmd c( "ps ax | grep -w /usr/sbin/hald | grep -v grep" );
	if( c.numLines()>0 )
	    {
	    extractNthWord( 0, *c.getLine(0) ) >> hald_pid;
	    y2mil( "hald_pid:" << hald_pid );
	    }
	if( hald_pid>0 )
	    {
	    ret = kill( hald_pid, SIGSTOP );
	    y2mil( "ret kill:" << ret << " pid:" << hald_pid );
	    }
	}
    else if( !stop && hald_pid>0 )
	{
	ret = kill( hald_pid, SIGCONT );
	y2mil( "ret kill:" << ret << " pid:" << hald_pid );
	}
    }


static bool notLoop( const Container& c ) { return( c.type()!=LOOP ); }
static bool fstabAdded( const Volume& v ) { return( v.fstabAdded()); }

int Storage::commit()
    {
    assertInit();
    lastAction.clear();
    extendedError.clear();
    SystemCmd c;
    CPair p = cPair( notLoop );
    int ret = 0;
    y2milestone( "empty:%d", p.empty() );
    if( !p.empty() )
	{
	ret = commitPair( p, notLoop );
	}
    p = cPair( isLoop );
    y2milestone( "empty:%d", p.empty() );
    if( ret==0 && !p.empty() )
	{
	ret = commitPair( p, isLoop );
	}
    VPair vp = vPair( fstabAdded );
    for( VolIterator i=vp.begin(); i!=vp.end(); ++i )
	i->setFstabAdded(false);
    if( ret!=0 )
	dumpObjectList();
    y2milestone( "ret:%d", ret );
    return( ret );
    }

bool 
Storage::ignoreError( list<commitAction*>::iterator i,
                      list<commitAction*>& al )
    {
    bool ret = false;
    if( !(*i)->container && (*i)->type==DISK && (*i)->stage==DECREASE )
	{
	++i;
	while( ret==false && i!=al.end() )
	    {
	    y2mil( "it:" << **i );
	    ret = (*i)->container && (*i)->type==DISK && (*i)->stage==DECREASE;
	    ++i;
	    }
	}
    y2mil( "ret:" << ret );
    return( ret );
    }

int
Storage::commitPair( CPair& p, bool (* fnc)( const Container& ) )
    {
    int ret = 0;
    y2milestone( "p.length:%d", p.length() );
    CommitStage a[] = { DECREASE, INCREASE, FORMAT, MOUNT };
    CommitStage* pt = a;
    while( unsigned(pt-a) < lengthof(a) )
	{
	bool new_pair = false;
	list<Container*> colist;
	list<Volume*> vlist;
	ContIterator i = p.begin();
	while( ret==0 && i != p.end() )
	    {
	    ret = i->getToCommit( *pt, colist, vlist );
	    ++i;
	    }
#if 0
	if( *pt == FORMAT && instsys() )
	    {
	    activateHld( true );
	    }
#endif
	list<commitAction*> todo;
	sortCommitLists( *pt, colist, vlist, todo );
	list<commitAction*>::iterator ac = todo.begin();
	while( ret==0 && ac != todo.end() )
	    {
	    bool cont = (*ac)->container;
	    CType type = (*ac)->type;
	    Container *co = cont ? const_cast<Container*>((*ac)->co()) : 
	                           const_cast<Container*>((*ac)->vol()->getContainer());
	    if( cont )
		{
		bool cont_removed = co->deleted() && type==LVM;
		ret = co->commitChanges( *pt );
		cont_removed = cont_removed && ret==0;
		if( cont_removed )
		    {
		    removeContainer( co );
		    new_pair = true;
		    }
		}
	    else
		{
		ret = co->commitChanges( *pt, const_cast<Volume*>((*ac)->vol()) );
		}
	    if( ret!=0 )
		{
		y2mil( "err at " << **ac );
		if( ignoreError( ac, todo ))
		    ret = 0;
		}
	    delete( *ac );
	    ++ac;
	    }
	y2milestone( "stage:%d new_pair:%d", *pt, new_pair );
	if( new_pair )
	    {
	    p = cPair( fnc );
	    new_pair = false;
	    }
	pt++;
	if( !todo.empty() )
	    {
	    SystemCmd c;
	    c.execute(DMSETUPBIN " ls");
	    c.execute(DMSETUPBIN " table");
	    c.execute(DMSETUPBIN " info");
	    logProcData();
	    }
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }


static bool isDmContainer( const Container& co )
{
    return co.type()==DM || co.type()==LVM || co.type()==DMRAID || co.type()==DMMULTIPATH;
}


bool Storage::removeDmMapsTo( const string& dev )
    {
    bool ret = false;
    y2mil("dev:" << dev);
    VPair vp = vPair( isDmContainer );
    for( VolIterator v=vp.begin(); v!=vp.end(); ++v )
	{
	Dm * dm = dynamic_cast<Dm *>(&(*v));
	if (dm != NULL)
	    {
	    y2mil( "dm:" << *dm );
	    if( dm->mapsTo( dev ) )
		{
		dm->removeTable();
		if( !dm->created() )
		    {
		    dm->setSilent();
		    dm->setDeleted();
		    }
		ret = true;
		}
	    }
	else
	    y2warning( "not a Dm descendant %s", v->device().c_str() );
	}
    VolIterator v;
    DiskIterator d;
    if( findVolume( dev, v ))
	{
	v->triggerUdevUpdate();
	}
    else if( (d=findDisk( dev ))!=dEnd())
	{
	d->triggerUdevUpdate();
	}
    waitForDevice();
    y2milestone( "ret:%d", ret );
    return( ret );
    }

void Storage::updateDmEmptyPeMap()
    {
    VPair vp = vPair( isDmContainer );
    for( VolIterator v=vp.begin(); v!=vp.end(); ++v )
	{
	Dm * dm = dynamic_cast<Dm *>(&(*v));
	if( dm!=NULL )
	    {
	    if( dm->getPeMap().empty() )
		{
		y2mil( "dm:" << *dm );
		dm->getTableInfo();
		dm->updateMajorMinor();
		}
	    }
	else
	    y2warning( "not a Dm descendant %s", v->device().c_str() );
	}
    }

bool Storage::checkDmMapsTo( const string& dev )
    {
    bool ret = false;
    y2milestone( "dev:%s", dev.c_str() );
    VPair vp = vPair( isDmContainer );
    VolIterator v=vp.begin(); 
    while( !ret && v!=vp.end() )
	{
	Dm * dm = dynamic_cast<Dm *>(&(*v));
	if( dm!=NULL )
	    ret = ret && dm->mapsTo( dev );
	++v;
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

void Storage::changeDeviceName( const string& old, const string& nw )
    {
    y2mil( "old:" << old << " new:" << nw );
    CPair p = cPair();
    ContIterator ci = p.begin();
    while( ci!=p.end() )
	{
	ci->changeDeviceName( old, nw );
	++ci;
	}
    }


void
Storage::getDiskList( bool (* CheckFnc)( const Disk& ), std::list<Disk*>& dl )
    {
    dl.clear();
    DiskPair dp = dPair( CheckFnc );
    DiskIterator i = dp.begin();
    while( i!=dp.end() )
	{
	y2mil( "disk:" << i->device() );
	dl.push_back( &(*i) );
	++i;
	}
    }

static bool showContainers( const Container& c )
    { return( !c.deleted()||c.type()==DISK ); }

void
Storage::getContainers( deque<ContainerInfo>& infos )
    {
    infos.clear ();
    assertInit();
    ConstContPair p = contPair( showContainers );
    for( ConstContIterator i = p.begin(); i != p.end(); ++i)
	{
	y2mil( "co:" << *i );
	infos.push_back( ContainerInfo() );
	i->getInfo( infos.back() );
	}
    }

void
Storage::getVolumes( deque<VolumeInfo>& infos )
    {
    infos.clear ();
    assertInit();
    ConstVolPair p = volPair( Volume::notDeleted );
    for( ConstVolIterator i = p.begin(); i != p.end(); ++i)
	{
	infos.push_back( VolumeInfo() );
	i->getInfo( infos.back() );
	}
    }

int 
Storage::getContVolInfo( const string& device, ContVolInfo& info)
    {
    int ret = 0;
    string dev = device;
    ContIterator c;
    VolIterator v;
    info.type = CUNKNOWN;
    assertInit();
    if( findVolume( dev, c, v ))
	{
	info.type = c->type();
	info.cname = c->device();
	info.vname = v->name();
	info.numeric = v->isNumeric();
	if( info.numeric )
	    info.nr = v->nr();
	else
	    info.nr = 0;
	}
    else 
	{
	DiskIterator d;
	DmraidCoIterator r;
	DmmultipathCoIterator m;
	std::pair<string,unsigned> p = Disk::getDiskPartition( dev );
	if( p.first=="/dev/md" )
	    {
	    info.cname = p.first;
	    info.vname = undevDevice(device);
	    info.type = MD;
	    info.numeric = true;
	    info.nr = p.second;
	    }
	else if( p.first=="/dev/loop" )
	    {
	    info.cname = p.first;
	    info.vname = undevDevice(device);
	    info.type = LOOP;
	    info.numeric = true;
	    info.nr = p.second;
	    }
	else if( p.first=="/dev/dm-" )
	    {
	    info.cname = p.first;
	    info.vname = undevDevice(device);
	    info.type = DM;
	    info.numeric = true;
	    info.nr = p.second;
	    }
	else if( (d=findDisk(p.first))!=dEnd() )
	    {
	    info.cname = d->device();
	    info.vname = dev.substr( dev.find_last_of('/')+1 );
	    info.type = DISK;
	    info.numeric = true;
	    info.nr = p.second;
	    }
	else if( (r=findDmraidCo(p.first))!=dmrCoEnd() )
	    {
	    info.cname = r->device();
	    info.vname = dev.substr( dev.find_last_of('/')+1 );
	    info.type = DMRAID;
	    info.numeric = true;
	    info.nr = p.second;
	    }
	else if( (m=findDmmultipathCo(p.first))!=dmmCoEnd() )
	    {
	    info.cname = m->device();
	    info.vname = dev.substr( dev.find_last_of('/')+1 );
	    info.type = DMMULTIPATH;
	    info.numeric = true;
	    info.nr = p.second;
	    }
	else if( dev.find("/dev/disk/by-uuid/")==0 ||
	         dev.find("/dev/disk/by-label/")==0 ||
	         dev.find("UUID=")==0 || dev.find("LABEL=")==0 )
	    {
	    if( dev[0] == '/' )
		{
		bool uuid = dev.find( "/by-uuid/" )!=string::npos;
		dev.erase( 0, dev.find_last_of('/')+1 );
		dev = (uuid?"UUID=":"LABEL=")+dev;
		}
	    if( findVolume(dev, v) )
		{
		info.type = v->cType();
		info.numeric = v->isNumeric();
		if( info.numeric )
		    info.nr = v->nr();
		info.vname = v->name();
		info.cname = v->getContainer()->name();
		}
	    }
	else if( (dev.find("/dev/disk/by-id/")==0 &&
	          (d=findDiskId(p.first))!=dEnd()) ||
		 (dev.find("/dev/disk/by-path/")==0 &&
		  (d=findDiskPath(p.first))!=dEnd()) )
	    {
	    info.type = DISK;
	    info.numeric = true;
	    info.nr = p.second;
	    info.cname = d->device();
	    if( p.second>0 )
		info.vname = Disk::getPartName( d->name(), p.second );
	    else
		info.vname = d->name();
	    if( info.vname.find('/')!=string::npos )
		info.vname.erase( 0, info.vname.find_last_of('/')+1 );
	    }
	else if( splitString( dev, "/" ).size()==3 && !Disk::needP( dev ) )
	    {
	    info.type = LVM;
	    info.numeric = false;
	    info.vname = dev.substr( dev.find_last_of('/')+1 );
	    info.cname = dev.substr( 0, dev.find_last_of('/') );
	    }
	else
	    {
	    info.cname = p.first;
	    info.vname = dev.substr( dev.find_last_of('/')+1 );
	    info.numeric = true;
	    info.nr = p.second;
	    }
	}
    y2mil( "dev:" << dev << " ret:" << ret << " cn:" << info.cname << 
           " vn:" << info.vname );
    if( info.numeric )
	y2mil( "nr:" << info.nr );
    return( ret );
    }

int
Storage::getVolume( const string& device, VolumeInfo& info )
    {
    int ret = 0;
    VolIterator v;
    if( findVolume( device, v ))
	{
	v->getInfo( info );
	}
    else
	{
	ret = STORAGE_VOLUME_NOT_FOUND;
	}
    return( ret );
    }

int Storage::getDiskInfo( const string& disk, DiskInfo& info )
    {
    int ret = 0;
    assertInit();
    DiskIterator i = findDisk( disk );
    if( i != dEnd() )
	{
	i->getInfo( info );
	}
    else
	ret = STORAGE_DISK_NOT_FOUND;
    return( ret );
    }

int Storage::getContDiskInfo( const string& disk, ContainerInfo& cinfo,
			      DiskInfo& info )
    {
    int ret = 0;
    assertInit();
    DiskIterator i = findDisk( disk );
    if( i != dEnd() )
	{
	((const Container*)&(*i))->getInfo( cinfo );
	i->getInfo( info );
	}
    else
	ret = STORAGE_DISK_NOT_FOUND;
    return( ret );
    }

int Storage::getPartitionInfo( const string& disk,
			       deque<storage::PartitionInfo>& plist )
    {
    int ret = 0;
    plist.clear();
    assertInit();
    DiskIterator i = findDisk( disk );
    if( i != dEnd() )
	{
	Disk::PartPair p = i->partPair (Disk::notDeleted);
	for (Disk::PartIter i2 = p.begin(); i2 != p.end(); ++i2)
	    {
	    plist.push_back( PartitionInfo() );
	    i2->getInfo( plist.back() );
	    }
	}
    else
	ret = STORAGE_DISK_NOT_FOUND;
    return( ret );
    }

int Storage::getLvmVgInfo( const string& name, LvmVgInfo& info )
    {
    int ret = 0;
    assertInit();
    LvmVgIterator i = findLvmVg( name );
    if( i != lvgEnd() )
	{
	i->getInfo( info );
	}
    else
	ret = STORAGE_LVM_VG_NOT_FOUND;
    return( ret );
    }

int Storage::getContLvmVgInfo( const string& name, ContainerInfo& cinfo,
                               LvmVgInfo& info )
    {
    int ret = 0;
    assertInit();
    LvmVgIterator i = findLvmVg( name );
    if( i != lvgEnd() )
	{
	((const Container*)&(*i))->getInfo( cinfo );
	i->getInfo( info );
	}
    else
	ret = STORAGE_LVM_VG_NOT_FOUND;
    return( ret );
    }

int Storage::getLvmLvInfo( const string& name,
			   deque<storage::LvmLvInfo>& plist )
    {
    int ret = 0;
    plist.clear();
    assertInit();
    LvmVgIterator i = findLvmVg( name );
    if( i != lvgEnd() )
	{
	LvmVg::LvmLvPair p = i->lvmLvPair(LvmVg::lvNotDeleted);
	for( LvmVg::LvmLvIter i2 = p.begin(); i2 != p.end(); ++i2)
	    {
	    plist.push_back( LvmLvInfo() );
	    i2->getInfo( plist.back() );
	    }
	}
    else
	ret = STORAGE_LVM_VG_NOT_FOUND;
    return( ret );
    }


int Storage::getDmraidCoInfo( const string& name, DmraidCoInfo& info )
    {
    int ret = 0;
    assertInit();
    DmraidCoIterator i = findDmraidCo( name );
    if( i != dmrCoEnd() )
	{
	i->getInfo( info );
	}
    else
	ret = STORAGE_DMRAID_CO_NOT_FOUND;
    return( ret );
    }

int Storage::getContDmraidCoInfo( const string& name, ContainerInfo& cinfo,
			          DmraidCoInfo& info )
    {
    int ret = 0;
    assertInit();
    DmraidCoIterator i = findDmraidCo( name );
    if( i != dmrCoEnd() )
	{
	((const Container*)&(*i))->getInfo( cinfo );
	i->getInfo( info );
	}
    else
	ret = STORAGE_DMRAID_CO_NOT_FOUND;
    return( ret );
    }


int
Storage::getDmmultipathCoInfo( const string& name, DmmultipathCoInfo& info )
{
    int ret = 0;
    assertInit();
    DmmultipathCoIterator i = findDmmultipathCo( name );
    if( i != dmmCoEnd() )
    {
	i->getInfo( info );
    }
    else
	ret = STORAGE_DMMULTIPATH_CO_NOT_FOUND;
    return( ret );
}

int
Storage::getContDmmultipathCoInfo( const string& name, ContainerInfo& cinfo,
				   DmmultipathCoInfo& info )
{
    int ret = 0;
    assertInit();
    DmmultipathCoIterator i = findDmmultipathCo( name );
    if( i != dmmCoEnd() )
    {
	((const Container*)&(*i))->getInfo( cinfo );
	i->getInfo( info );
    }
    else
	ret = STORAGE_DMMULTIPATH_CO_NOT_FOUND;
    return( ret );
}


int Storage::getMdInfo( deque<storage::MdInfo>& plist )
    {
    int ret = 0;
    plist.clear();
    assertInit();
    ConstMdPair p = mdPair(Md::notDeleted);
    for( ConstMdIterator i = p.begin(); i != p.end(); ++i )
	{
	plist.push_back( MdInfo() );
	i->getInfo( plist.back() );
	}
    return( ret );
    }

int Storage::getNfsInfo( deque<storage::NfsInfo>& plist )
    {
    int ret = 0;
    plist.clear();
    assertInit();
    ConstNfsPair p = nfsPair(Nfs::notDeleted);
    for( ConstNfsIterator i = p.begin(); i != p.end(); ++i )
	{
	plist.push_back( NfsInfo() );
	i->getInfo( plist.back() );
	}
    return( ret );
    }

int Storage::getLoopInfo( deque<storage::LoopInfo>& plist )
    {
    int ret = 0;
    plist.clear();
    assertInit();
    ConstLoopPair p = loopPair(Loop::notDeleted);
    for( ConstLoopIterator i = p.begin(); i != p.end(); ++i )
	{
	plist.push_back( LoopInfo() );
	i->getInfo( plist.back() );
	}
    return( ret );
    }

int Storage::getDmInfo( deque<storage::DmInfo>& plist )
    {
    int ret = 0;
    plist.clear();
    assertInit();
    ConstDmPair p = dmPair(Dm::notDeleted);
    for( ConstDmIterator i = p.begin(); i != p.end(); ++i )
	{
	plist.push_back( DmInfo() );
	i->getInfo( plist.back() );
	}
    return( ret );
    }

int Storage::getDmraidInfo( const string& name,
                            deque<storage::DmraidInfo>& plist )
    {
    int ret = 0;
    plist.clear();
    assertInit();
    DmraidCoIterator i = findDmraidCo( name );
    if( i != dmrCoEnd() )
	{
	DmraidCo::DmraidPair p = i->dmraidPair(DmraidCo::raidNotDeleted);
	for( DmraidCo::DmraidIter i2 = p.begin(); i2 != p.end(); ++i2 )
	    {
	    plist.push_back( DmraidInfo() );
	    i2->getInfo( plist.back() );
	    }
	}
    else
	ret = STORAGE_DMRAID_CO_NOT_FOUND;
    return( ret );
    }


int
Storage::getDmmultipathInfo( const string& name,
			     deque<storage::DmmultipathInfo>& plist )
{
    int ret = 0;
    plist.clear();
    assertInit();
    DmmultipathCoIterator i = findDmmultipathCo( name );
    if( i != dmmCoEnd() )
    {
	DmmultipathCo::DmmultipathPair p = i->dmmultipathPair(DmmultipathCo::multipathNotDeleted);
	for( DmmultipathCo::DmmultipathIter i2 = p.begin(); i2 != p.end(); ++i2 )
	{
	    plist.push_back( DmmultipathInfo() );
	    i2->getInfo( plist.back() );
	}
    }
    else
	ret = STORAGE_DMMULTIPATH_CO_NOT_FOUND;
    return( ret );
}


list<string> Storage::getAllUsedFs() const 
{
    set<FsType> fs;
    ConstVolPair p = volPair( Volume::notDeleted );
    for( ConstVolIterator v=p.begin(); v!=p.end(); ++v )
    {
	FsType t = v->getFs();
	if (t!=FSUNKNOWN && t!=FSNONE)
	    fs.insert(t);
    }
    list<string> ret;
    for( set<FsType>::const_iterator i=fs.begin(); i!=fs.end(); ++i )
    {
	ret.push_back(Volume::fsTypeString(*i));
    }
    y2mil( "ret:" << ret );
    return ret;
}


bool
Storage::getFsCapabilities (FsType fstype, FsCapabilities& fscapabilities) const
{
    struct FsCapabilitiesX : public FsCapabilities
    {
	FsCapabilitiesX (bool isExtendableX, bool isExtendableWhileMountedX,
			 bool isReduceableX, bool isReduceableWhileMountedX,
			 bool supportsUuidX, bool supportsLabelX,
			 bool labelWhileMountedX, unsigned int labelLengthX,
			 unsigned long long minimalFsSizeKX)
	    : FsCapabilities ()
	{
	    isExtendable = isExtendableX;
	    isExtendableWhileMounted = isExtendableWhileMountedX;
	    isReduceable = isReduceableX;
	    isReduceableWhileMounted = isReduceableWhileMountedX;
	    supportsUuid = supportsUuidX;
	    supportsLabel = supportsLabelX;
	    labelWhileMounted = labelWhileMountedX;
	    labelLength = labelLengthX;
	    minimalFsSizeK = minimalFsSizeKX;
	}
    };

    static FsCapabilitiesX reiserfsCaps (true, true, true, false, true, true,
					 false, 16, 50*1024);

    static FsCapabilitiesX ext2Caps (true, false, true, false, true, true,
				     true, 16, 16);

    static FsCapabilitiesX ext3Caps (true, true, true, false, true, true,
				     true, 16, 10*1024);

    static FsCapabilitiesX xfsCaps (true, true, false, false, true, true,
				    false, 12, 40*1024);

    static FsCapabilitiesX ntfsCaps (true, false, true, false, false, false,
				     false, 0, 10*1024);

    static FsCapabilitiesX fatCaps (true, false, true, false, false, false,
				    false, 0, 16);

    static FsCapabilitiesX swapCaps (true, false, true, false, true, true,
				     true, 16, 64);

    static FsCapabilitiesX jfsCaps (false, false, false, false, true, true,
				    false, 16, 16*1024);

    static FsCapabilitiesX hfsCaps (false, false, true, false, false, false,
				    false, 0, 10*1024);

    static FsCapabilitiesX hfspCaps(false, false, true, false, false, false,
				    false, 0, 10*1024);

    static FsCapabilitiesX nfsCaps (false, false, false, false, false, false,
				    false, 0, 10*1024);

    switch (fstype)
    {
	case REISERFS:
	    fscapabilities = reiserfsCaps;
	    return true;

	case EXT2:
	    fscapabilities = ext2Caps;
	    return true;

	case EXT3:
	    fscapabilities = ext3Caps;
	    return true;

	case XFS:
	    fscapabilities = xfsCaps;
	    return true;

	case VFAT:
	    fscapabilities = fatCaps;
	    return true;

	case NTFS:
	    fscapabilities = ntfsCaps;
	    return true;

	case JFS:
	    fscapabilities = jfsCaps;
	    return true;

	case HFS:
	    fscapabilities = hfsCaps;
	    return true;

	case HFSPLUS:
	    fscapabilities = hfspCaps;
	    return true;

	case NFS:
	    fscapabilities = nfsCaps;
	    return true;

	case SWAP:
	    fscapabilities = swapCaps;
	    return true;

	default:
	    return false;
    }
}


void Storage::removeDmTableTo( const Volume& vol )
    {
    if( vol.cType()==DISK || vol.cType()==MD )
	{
	removeDmTable( Dm::devToTable(vol.device()));
	y2mil( "dev:" << vol.device() );
	removeDmMapsTo( vol.device() );
	if( vol.cType()==DISK )
	    {
	    removeDmMapsTo( vol.getContainer()->device() );
	    if( vol.getContainer()->majorNr()>0 )
		{
		string cmd = DMSETUPBIN " table | grep -w ";
		cmd += decString(vol.getContainer()->majorNr()) + ":" +
		       decString(vol.getContainer()->minorNr());
		cmd += " | sed s/:.*// | uniq";
		SystemCmd c( cmd );
		unsigned line=0;
		while( line<c.numLines() )
		    {
		    removeDmTable( *c.getLine(line) );
		    line++;
		    }
		}
	    }
	}
    }
    
void Storage::removeDmTableTo( const string& device )
    {
    y2mil( "dev:" << device );
    VolIterator vol;
    if( findVolume( device, vol ))
	removeDmTableTo( *vol );
    }
    
bool Storage::removeDmTable( const string& table )
    {
    SystemCmd c(DMSETUPBIN " table " + quote(table));
    bool ret = false;
    if( c.retcode()==0 )
	{
	c.execute(DMSETUPBIN " info " + quote(table));
	c.execute(DMSETUPBIN " remove " + quote(table));
	waitForDevice();
	ret = c.retcode()==0;
	c.execute(DMSETUPBIN " table | grep " + quote(table));
	logProcData();
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

void
Storage::logCo( const string& device )
    {
    ContIterator cc;
    if( findContainer( device, cc ))
	logCo( &(*cc) );
    else
	y2mil( "not found:" << device );
    }

void
Storage::logCo( Container* c ) const
    {
    std::ostringstream b;
    classic(b);
    c->print( b );
    y2mil( "log co:" << b.str() );
    for( Container::ConstPlainIterator i=c->begin(); i!=c->end(); ++i )
	{
	b.str("");
	(*i)->print( b );
	y2mil( "log vo:" << b.str() );
	}
    }

void Storage::logProcData( const string& l )
    {
    y2mil( "begin:" << l );
    ProcPart t;
    AsciiFile md( "/proc/mdstat" );
    for( unsigned i=0; i<md.numLines(); i++ )
	y2mil( "mdstat:" << i+1 << ". line:" << md[i] );
    AsciiFile mo( "/proc/mounts" );
    for( unsigned i=0; i<mo.numLines(); i++ )
	y2mil( "mounts:" << i+1 << ". line:" << mo[i] );
    y2mil( "end" << l );
    }


bool Storage::findVolume( const string& device, ContIterator& c,
                          VolIterator& v )
    {
    bool ret = false;
    if( findVolume( device, v ))
	{
	const Container *co = v->getContainer();
	CPair cp = cPair();
	c = cp.begin();
	while( c!=cp.end() && &(*c)!=co )
	    ++c;
	ret = c!=cp.end();
	}
    y2milestone( "device:%s ret:%d c->device:%s v->device:%s", device.c_str(),
                 ret, ret?c->device().c_str():"nil",
		 ret?v->device().c_str():"nil" );
    return( ret );
    }

bool Storage::findDm( const string& device, const Dm*& dm )
    {
    bool ret = false;
    ConstDmPair p = dmPair();
    ConstDmIterator i = p.begin();
    while( !ret && i!=p.end() )
	{
	ret = i->device()==device ||
	      find( i->altNames().begin(), i->altNames().end(), device )!=
		  i->altNames().end();
	if( !ret )
	    ++i;
	}
    if( ret )
	{
	dm = &(*i);
	y2mil( "dm:" << *dm );
	}
    y2mil( "device:" << device << " ret:" << ret );
    return( ret );
    }

bool Storage::findDmUsing( const string& device, const Dm*& dm )
    {
    bool ret = false;
    ConstDmPair p = dmPair();
    ConstDmIterator i = p.begin();
    while( !ret && i!=p.end() )
	{
	ret = i->usingPe(device);
	if( !ret )
	    ++i;
	}
    if( ret )
	{
	dm = &(*i);
	y2mil( "dm:" << *dm );
	}
    y2mil( "device:" << device << " ret:" << ret );
    return( ret );
    }

bool Storage::removeDm( const string& device )
    {
    const Dm* dm = 0;
    if( findDm( device, dm ))
	{
	ContIterator c;
	if( findContainer( dm->getContainer()->device(), c ))
	    {
	    c->removeFromList( const_cast<Dm*>(dm) );
	    if( c->isEmpty() )
		removeContainer( &(*c), true );
	    }
	}
    y2mil( "device:" << device << " ret:" << (dm!=0)  );
    return( dm!=0 );
    }

bool Storage::findContainer( const string& device, ContIterator& c )
    {
    CPair cp = cPair();
    c = cp.begin();
    while( c!=cp.end() && c->device()!=device )
	++c;
    return( c!=cp.end() );
    }

bool Storage::findVolume( const string& device, VolIterator& v, bool also_del )
    {
    assertInit();
    string label;
    string uuid;
    string d;
    if( device.find( "LABEL=" )==0 )
	label = device.substr( 6 );
    else if( device.find( "UUID=" )==0 )
	uuid = device.substr( 5 );
    else
	d = normalizeDevice( device );
    if( !label.empty() || !uuid.empty() )
	y2milestone( "label:%s uuid:%s", label.c_str(), uuid.c_str() );
    VPair p = vPair( also_del?NULL:Volume::notDeleted );
    v = p.begin();
    if( label.empty() && uuid.empty() )
	{
	while( v!=p.end() && v->device()!=d )
	    {
	    const list<string>& al( v->altNames() );
	    if( find( al.begin(), al.end(), d )!=al.end() )
		break;
	    ++v;
	    }
	if( !p.empty() && v==p.end() && d.find("/dev/loop")==0 )
	    {
	    v = p.begin();
	    while( v!=p.end() && v->loopDevice()!=d )
		++v;
	    }
	if( !p.empty() && v==p.end() && d.find("/dev/mapper/cr_")==0 )
	    {
	    v = p.begin();
	    while( v!=p.end() && v->dmcryptDevice()!=d )
		++v;
	    }
	if( !p.empty() && v==p.end() )
	    {
	    d.replace( 0, 5, "/dev/mapper/" );
	    v = p.begin();
	    while( v!=p.end() && v->device()!=d )
		++v;
	    }
	}
    else if( !label.empty() )
	{
	while( v!=p.end() && v->getLabel()!=label )
	    ++v;
	}
    else if( !uuid.empty() )
	{
	while( v!=p.end() && v->getUuid()!=uuid )
	    ++v;
	}
    return( v!=p.end() );
    }

bool Storage::findVolume( const string& device, Volume const * &vol )
    {
    bool ret = false;
    vol = NULL;
    VolIterator v;
    if( findVolume( device, v ))
	{
	vol = &(*v);
	ret = true;
	}
    y2mil( "device:" << device << " ret:" << ret );
    if( vol )
	y2mil( "vol:" << *vol );
    return( ret );
    }

string Storage::findNormalDevice( const string& device )
    {
    string ret;
    VolIterator v;
    if( findVolume( device, v ))
	ret = v->device();
    y2mil( "device:" << device << " ret:" << ret );
    return( ret );
    }


bool Storage::clearUsedBy(const string& dev)
{
    return setUsedBy(dev, UB_NONE, "");
}


bool Storage::setUsedBy(const string& dev, UsedByType ub_type, const string& ub_name)
{
    bool ret=true;
    VolIterator v;
    if( !findVolume( dev, v ) )
    {
	DiskIterator i = findDisk( dev );
	if( i != dEnd() )
	{
	    i->setUsedBy(ub_type, ub_name);
	}
	else
	{
	    ret = false;
	    y2err("could not set ub_type:" << ub_type << " ub_name:" << ub_name <<
		  "for dev: " << dev);
	}
    }
    else
    {
	v->setUsedBy(ub_type, ub_name);
    }
    y2mil("dev:" << dev << " ub_type:" << ub_type << " ub_name:" << ub_name << " ret:" << ret);
    return ret;
}


bool Storage::usedBy( const string& dev, storage::usedBy& ub )
    {
    ub.clear();
    bool ret=false;
    VolIterator v;
    if( !findVolume( dev, v ) )
	{
	DiskIterator i = findDisk( dev );
	if( i != dEnd() )
	    {
	    ub = i->getUsedBy();
	    ret = true;
	    }
	}
    else
	{
	ub = v->getUsedBy();
	ret = true;
	}
    y2mil( "dev:" << dev << " ret:" << ret << " ub:" << ub );
    return( ret );
    }

UsedByType Storage::usedBy( const string& dev )
    {
    storage::usedBy ub;
    usedBy( dev, ub );
    return( ub.type() );
    }

void Storage::progressBarCb( const string& id, unsigned cur, unsigned max )
    {
    y2milestone( "id:%s cur:%d max:%d", id.c_str(), cur, max );
    CallbackProgressBar cb = getCallbackProgressBarTheOne();
    if( cb )
	(*cb)( id, cur, max );
    }

void Storage::showInfoCb( const string& info )
    {
    y2milestone( "INSTALL INFO:%s", info.c_str() );
    CallbackShowInstallInfo cb = getCallbackShowInstallInfoTheOne();
    lastAction = info;
    if( cb )
	(*cb)( info );
    }

void Storage::infoPopupCb( const string& info )
    {
    y2milestone( "INFO POPUP:%s", info.c_str() );
    CallbackInfoPopup cb = getCallbackInfoPopupTheOne();
    if( cb )
	(*cb)( info );
    }

void Storage::addInfoPopupText( const string& disk, const string txt )
    {
    y2mil( "d:" << disk << " txt:" << txt );
    infoPopupTxts.push_back( make_pair(disk,txt) );
    }

bool Storage::yesnoPopupCb( const string& info )
    {
    y2milestone( "YESNO POPUP:%s", info.c_str() );
    CallbackYesNoPopup cb = getCallbackYesNoPopupTheOne();
    if( cb )
	return (*cb)( info );
    else
	return( true );
    }

Storage::DiskIterator Storage::findDisk( const string& disk )
    {
    assertInit();
    string d = normalizeDevice( disk );
    DiskPair p = dPair();
    DiskIterator ret=p.begin();
    while( ret != p.end() && ret->device()!=d )
	++ret;
    return( ret );
    }

Storage::DiskIterator Storage::findDiskId( const string& id )
    {
    assertInit();
    string val = id;
    if( val.find( '/' )!=string::npos )
	val.erase( 0, val.find_last_of('/')+1 );
    DiskPair p = dPair();
    DiskIterator ret=p.begin();
    bool found = false;
    while( ret != p.end() && !found )
	{
	const std::list<string>& ul( ret->udevId() );
	found = find(ul.begin(),ul.end(),val) != ul.end();
	if( !found )
	    ++ret;
	}
    y2mil( "ret:" << (ret!=p.end()?ret->device():"NULL") );
    return( ret );
    }

Storage::DiskIterator Storage::findDiskPath( const string& path )
    {
    assertInit();
    string val = path;
    if( val.find( '/' )!=string::npos )
	val.erase( 0, val.find_last_of('/')+1 );
    DiskPair p = dPair();
    DiskIterator ret=p.begin();
    while( ret != p.end() && ret->udevPath()!=val )
	++ret;
    y2mil( "ret:" << (ret!=p.end()?ret->device():"NULL") );
    return( ret );
    }

Storage::LvmVgIterator Storage::findLvmVg( const string& name )
    {
    assertInit();
    LvmVgPair p = lvgPair();
    LvmVgIterator ret=p.begin();
    while( ret != p.end() && (ret->deleted() || ret->name()!=name) )
	++ret;
    return( ret );
    }

Storage::DmraidCoIterator Storage::findDmraidCo( const string& name )
    {
    assertInit();
    DmraidCoPair p = dmrCoPair();
    DmraidCoIterator ret=p.begin();
    string tname = DmPartCo::undevName(name);
    while( ret!=p.end() && (ret->deleted() || ret->name()!=tname))
	++ret;
    return( ret );
    }

Storage::DmmultipathCoIterator Storage::findDmmultipathCo( const string& name )
    {
    assertInit();
    DmmultipathCoPair p = dmmCoPair();
    DmmultipathCoIterator ret=p.begin();
    string tname = DmPartCo::undevName(name);
    while( ret!=p.end() && (ret->deleted() || ret->name()!=tname))
	++ret;
    return( ret );
    }

Storage::DmPartCoIterator Storage::findDmPartCo( const string& name )
    {
    assertInit();
    DmPartCoPair p = dmpCoPair();
    DmPartCoIterator ret=p.begin();
    string tname = DmPartCo::undevName(name);
    while( ret!=p.end() && (ret->deleted() || ret->name()!=tname))
	++ret;
    return( ret );
    }

bool Storage::knownDevice( const string& dev, bool disks_allowed )
    {
    bool ret=true;
    VolIterator v;
    if( !findVolume( dev, v ) )
	{
	ret = disks_allowed && findDisk( dev )!=dEnd();
	}
    y2milestone( "dev:%s ret:%d", dev.c_str(), ret );
    return( ret );
    }

bool Storage::setDmcryptData( const string& dev, const string& dm, 
                              unsigned dmnum, unsigned long long siz,
			      storage::EncryptType typ )
    {
    y2milestone( "dev:%s dm:%s dmn:%u sizeK:%llu", dev.c_str(), dm.c_str(), 
                 dmnum, siz );
    bool ret=false;
    VolIterator v;
    if( dm.find("/temporary-cryptsetup-")==string::npos && 
        findVolume( dev, v ) )
	{
	v->setDmcryptDevEnc( dm, typ, siz!=0 );
	v->replaceAltName( "/dev/dm-", Dm::dmDeviceName(dmnum) );
	v->setSize( siz );
	ret = true;
	}
    y2mil( "ret:" << ret );
    return( ret );
    }

bool Storage::deletedDevice( const string& dev )
    {
    VPair p = vPair( Volume::isDeleted );
    VolIterator v = p.begin();
    while( v!=p.end() && v->device()!=dev )
	++v;
    bool ret = v!=p.end();
    y2milestone( "dev:%s ret:%d", dev.c_str(), ret );
    return( ret );
    }

bool Storage::isDisk( const string& dev )
    {
    return( findDisk( dev ) != dEnd() );
    }


const Volume*
Storage::getVolume( const string& dev )
    {
    const Volume* ret=NULL;
    VolIterator v;
    if( findVolume( dev, v ) )
	{
	ret = &(*v);
	}
    y2milestone( "dev:%s ret:%s", dev.c_str(),
                 ret?ret->device().c_str():"nil" );
    return( ret );
    }

bool Storage::canUseDevice( const string& dev, bool disks_allowed )
    {
    bool ret=true;
    VolIterator v;
    if( !findVolume( dev, v ) )
	{
	if( disks_allowed )
	    {
	    DiskIterator i = findDisk( dev );
	    ret = i!=dEnd() && i->getUsedByType()==UB_NONE &&
	          i->numPartitions()==0;
	    }
	else
	    ret = false;
	}
    else
	{
	ret = v->canUseDevice();
	}
    y2milestone( "dev:%s ret:%d", dev.c_str(), ret );
    return( ret );
    }

string Storage::deviceByNumber( const string& majmin )
    {
    string ret="";
    string::size_type pos = majmin.find( ":" );
    if( pos!=string::npos )
	{
	unsigned ma, mi;
	majmin.substr( 0, pos ) >> ma;
	majmin.substr( pos+1 ) >> mi;
	ConstVolPair p = volPair( Volume::notDeleted );
	ConstVolIterator v = p.begin();
	while( v!=p.end() && (ma!=v->majorNr() || mi!=v->minorNr()))
	   {
	   ++v;
	   }
	if( v!=p.end() )
	    ret = v->device();
	if( ret.empty() )
	    {
	    ConstDiskPair d = diskPair();
	    ConstDiskIterator di = d.begin();
	    while( di!=d.end() && (ma!=di->majorNr() || mi!=di->minorNr()))
		++di;
	    if( di!=d.end() )
		ret = di->device();
	    }
	if( ret.empty() && ma==Dm::dmMajor())
	    {
	    ConstDmraidCoPair d = dmraidCoPair();
	    ConstDmraidCoIterator di = d.begin();
	    while( di!=d.end() && mi!=di->minorNr() )
		++di;
	    if( di!=d.end() )
		ret = di->device();
	    }
	}
    y2milestone( "majmin %s ret:%s", majmin.c_str(), ret.c_str() );
    return( ret );
    }

unsigned long long Storage::deviceSize( const string& dev )
    {
    unsigned long long ret=0;
    VolIterator v;
    if( !findVolume( dev, v ) )
	{
	DiskIterator i = findDisk( dev );
	if( i!=dEnd() )
	    ret = i->sizeK();
	}
    else
	ret = v->sizeK();
    y2milestone( "dev:%s ret:%llu", dev.c_str(), ret );
    return( ret );
    }

int Storage::removeContainer( Container* val, bool call_del )
    {
    y2milestone( "name:%s call_del:%d", val->name().c_str(), call_del );
    int ret = 0;
    CIter i=cont.begin();
    while( i!=cont.end() && *i!=val )
	++i;
    if( i!=cont.end() )
	{
	if( call_del )
	    delete( *i );
	cont.erase( i );
	}
    else
	{
	ret = STORAGE_CONTAINER_NOT_FOUND;
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }


int Storage::removeUsing(const string& device, const storage::usedBy& uby)
{
    y2mil("device:" << device << " uby:" << uby);
    int ret=0;
    switch( uby.type() )
	{
	case UB_MD:
	    ret = removeVolume(uby.device());
	    break;
	case UB_DM:
	    ret = removeVolume(uby.device());
	    break;
	case UB_LVM:
	    ret = removeLvmVg(uby.name());
	    break;
	case UB_DMRAID:
	    //ret = removeDmraidCo( name );
	    break;
	case UB_DMMULTIPATH:
	    break;
	case UB_NONE:
	    y2war(device << " used by none");
	    break;
	default:
	    ret = STORAGE_REMOVE_USING_UNKNOWN_TYPE;
	    break;
	}
    y2mil("ret:" << ret);
    return ret;
}


void Storage::rootMounted()
    {
    MdCo* md;
    root_mounted = true;
    if( !root().empty() )
	{
    	if( haveMd(md) )
	    md->syncRaidtab();
	if( instsys() )
	    {
	    string path = root()+"/etc/fstab";
	    unlink( path.c_str() );
	    }
	int ret = fstab->changeRootPrefix( root()+"/etc" );
	if( ret!=0 )
	    y2error( "changeRootPrefix returns %d", ret );
	}
    }

bool
Storage::checkDeviceMounted( const string& device, string& mp )
    {
    bool ret = false;
    assertInit();
    y2milestone( "device:%s", device.c_str() );
    VolIterator vol;
    ProcMounts mountData( this );
    if( findVolume( device, vol ) )
	{
	mp = mountData.getMount( vol->mountDevice() );
	if( mp.empty() )
	    mp = mountData.getMount( vol->altNames() );
	ret = !mp.empty();
	}
    else
	{
	mp = mountData.getMount( device );
	}
    y2milestone( "ret:%d mp:%s", ret, mp.c_str() );
    return( ret );
    }

bool
Storage::umountDevice( const string& device )
    {
    bool ret = false;
    assertInit();
    y2milestone( "device:%s", device.c_str() );
    VolIterator vol;
    if( !readonly && findVolume( device, vol ) )
	{
	if( vol->umount()==0 )
	    {
	    vol->crUnsetup();
	    ret = true;
	    }
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

bool
Storage::mountDev( const string& device, const string& mp, bool ro,
                   const string& opts )
    {
    bool ret = true;
    assertInit();
    y2milestone( "device:%s mp:%s ro:%d opts:%s", device.c_str(), mp.c_str(), 
                 ro, opts.c_str() );
    VolIterator vol;
    if( !readonly && findVolume( device, vol ) )
	{
	if( vol->needCrsetup() )
	    {
	    ret = vol->doCrsetup()==0;
	    }
	if( ret )
	    {
	    string save = vol->getFstabOption();
	    vol->setFstabOption( opts );
	    ret = vol->mount( mp, ro )==0;
	    vol->setFstabOption( save );
	    }
	if( !ret )
	    vol->crUnsetup();
	}
    else
	ret = false;
    y2milestone( "ret:%d", ret );
    return( ret );
    }

bool
Storage::readFstab( const string& dir, deque<VolumeInfo>& infos )
    {
    static deque<VolumeInfo> vil;
    static Regex disk_part( "^/dev/[sh]d[a-z]+[0-9]+$" );
    vil.clear();
    bool ret = false;
    VolIterator vol;
    assertInit();
    y2milestone( "dir:%s", dir.c_str() );
    EtcFstab *fstab = new EtcFstab( dir, true );
    list<FstabEntry> le;
    fstab->getEntries( le );
    for( list<FstabEntry>::const_iterator i=le.begin(); i!=le.end(); ++i )
	{
	y2mil( "entry:" << *i );
	VolumeInfo* info = NULL;
	if( disk_part.match( i->dentry ) )
	    {
	    info = new VolumeInfo;
	    info->create = info->format = info->resize = false;
	    info->sizeK = info->OrigSizeK = info->minor = info->major = 0;
	    info->device = i->dentry;
	    info->mount = i->mount;
	    info->mount_by = MOUNTBY_DEVICE;
	    info->fs = Volume::toFsType( i->fs );
	    info->fstab_options = mergeString( i->opts, "," );
	    vil.push_back( *info );
	    }
	else if( findVolume( i->dentry, vol ) )
	    {
	    info = new VolumeInfo;
	    vol->getInfo( *info );
	    vol->mergeFstabInfo( *info, *i );
	    y2mil( "volume:" << *vol );
	    vil.push_back( *info );
	    }
	if( info )
	    {
	    delete info;
	    info = NULL;
	    }
	}
    delete fstab;
    infos = vil;
    ret = !infos.empty();
    y2milestone( "ret:%d", ret );
    return( ret );
    }

unsigned long long 
Storage::getDfSize( const string& mp )
    {
    unsigned long long ret = 0;
    struct statvfs64 fsbuf;
    if( statvfs64( mp.c_str(), &fsbuf )==0 )
	{
	ret = fsbuf.f_blocks;
	ret *= fsbuf.f_bsize;
	ret /= 1024;
	y2mil("blocks:" << fsbuf.f_blocks << " free:" << fsbuf.f_bfree <<
	      " bsize:" << fsbuf.f_bsize);
	}
    else
	{
	y2war( "errno:" << errno << " " << strerror(errno));
	}
    y2mil( "mp:" << mp << " ret:" << ret );
    return( ret );
    }

bool
Storage::getFreeInfo( const string& device, unsigned long long& resize_free,
		      unsigned long long& df_free,
		      unsigned long long& used, bool& win, bool& efi,
		      bool use_cache )
    {
    bool ret = false;
    assertInit();
    resize_free = df_free = used = 0;
    y2milestone( "device:%s use_cache:%d", device.c_str(), use_cache );
    VolIterator vol;
    if( findVolume( device, vol ) )
	{
	if( use_cache && getFreeInf( vol->device(), df_free, resize_free,
	                             used, win, efi, ret ))
	    {
	    }
	else
	    {
	    bool needUmount = false;
	    string mp;
	    if( !vol->isMounted() )
		{
		removeDmTableTo( *vol );
		string mdir = tmpDir() + "/tmp_mp";
		unlink( mdir.c_str() );
		rmdir( mdir.c_str() );
		string save_opt;
		string cur_opt;
		if( vol->getFs()==NTFS )
		    {
		    save_opt = vol->getFstabOption();
		    cur_opt = save_opt;
		    if( !cur_opt.empty() )
			cur_opt += ",";
		    cur_opt += "show_sys_files";
		    vol->changeFstabOptions( cur_opt );
		    }
		if( vol->getFs()!=FSUNKNOWN && mkdir( mdir.c_str(), 0700 )==0 &&
		    mountDev( device, mdir ) )
		    {
		    needUmount = true;
		    mp = mdir;
		    }
		if( vol->getFs()==NTFS )
		    vol->changeFstabOptions( save_opt );
		}
	    else
		mp = vol->getMount();
	    if( !mp.empty() )
		{
		struct statvfs64 fsbuf;
		ret = statvfs64( mp.c_str(), &fsbuf )==0;
		if( ret )
		    {
		    df_free = fsbuf.f_bfree;
		    df_free *= fsbuf.f_bsize;
		    df_free /= 1024;
		    resize_free = df_free;
		    used = fsbuf.f_blocks-fsbuf.f_bfree;
		    used *= fsbuf.f_bsize;
		    used /= 1024;
		    y2mil("blocks:" << fsbuf.f_blocks << " free:" << fsbuf.f_bfree <<
			  " bsize:" << fsbuf.f_bsize);
		    y2mil("free:" << df_free << " used:" << used);
		    }
		if( ret && vol->getFs()==NTFS )
		    {
		    SystemCmd c( "ntfsresize -f -i " + quote(device));
		    string fstr = " might resize at ";
		    string::size_type pos;
		    if( c.retcode()==0 &&
			(pos=c.getString()->find( fstr ))!=string::npos )
			{
			y2milestone( "pos %zd", pos );
			pos = c.getString()->find_first_not_of( " \t\n", pos+fstr.size());
			y2milestone( "pos %zd", pos );
			string number = c.getString()->substr( pos,
							       c.getString()->find_first_not_of( "0123456789", pos ));
			y2milestone( "number \"%s\"", number.c_str() );
			unsigned long long t;
			number >> t;
			y2milestone( "number %llu", t );
			if( t-vol->sizeK()<resize_free )
			    resize_free = t-vol->sizeK();
			y2milestone( "resize_free %llu", t );
			}
		    else
			ret = false;
		    }
		win = false;
		const char * files[] = { "boot.ini", "msdos.sys", "io.sys",
				         "config.sys", "MSDOS.SYS", "IO.SYS",
					 "bootmgr", "$Boot" };
		string f;
		unsigned i=0;
		while( !win && i<lengthof(files) )
		    {
		    f = mp + "/" + files[i];
		    win = access( f.c_str(), R_OK )==0;
		    i++;
		    }
		efi = vol->getFs()==VFAT && checkDir( mp + "/efi" );
		if( efi )
		    win = false;
		}
	    if( needUmount )
		{
		umountDevice( device );
		rmdir( mp.c_str() );
		rmdir( tmpDir().c_str() );
		}

	    if( vol->needCrsetup() && vol->doCrsetup() )
		{
		ret = vol->mount( mp )==0;
		if( !ret )
		    vol->crUnsetup();
		}
	    setFreeInfo( vol->device(), df_free, resize_free, used, win, efi,
	                 ret );
	    }
	}
    if( ret )
	y2milestone( "resize_free:%llu df_free:%llu used:%llu",
	             resize_free, df_free, used );
    y2milestone( "ret:%d win:%d", ret, win );
    return( ret );
    }

void Storage::setFreeInfo( const string& device, unsigned long long df_free,
                           unsigned long long resize_free,
			   unsigned long long used, bool win, bool efi,
			   bool resize_ok )
    {
    y2milestone( "device:%s df_free:%llu resize_free:%llu used:%llu win:%d efi:%d",
		 device.c_str(), df_free, resize_free, used, win, efi );

    FreeInfo inf( df_free, resize_free, used, win, efi, resize_ok );
    freeInfo[device] = inf;
    }

bool
Storage::getFreeInf( const string& device, unsigned long long& df_free,
		     unsigned long long& resize_free,
		     unsigned long long& used, bool& win,  bool& efi,
		     bool& resize_ok )
    {
    map<string,FreeInfo>::iterator i = freeInfo.find( device );
    bool ret = i!=freeInfo.end();
    if( ret )
	{
	df_free = i->second.df_free;
	resize_free = i->second.resize_free;
	used = i->second.used;
	win = i->second.win;
	efi = i->second.efi;
	resize_ok = i->second.rok;
	}
    y2milestone( "device:%s ret:%d", device.c_str(), ret );
    if( ret )
	y2milestone( "df_free:%llu resize_free:%llu used:%llu win:%d efi:%d resize_ok:%d",
		     df_free, resize_free, used, win, efi, resize_ok );
    return( ret );
    }

void
Storage::eraseFreeInfo( const string& device )
    {
    map<string,FreeInfo>::iterator i = freeInfo.find( device );
    if( i!=freeInfo.end() )
	freeInfo.erase(i);
    }

int
Storage::createBackupState( const string& name )
    {
    int ret = readonly?STORAGE_CHANGE_READONLY:0;
    assertInit();
    y2milestone( "name:%s", name.c_str() );
    if( ret==0 )
	{
	if(checkBackupState(name))
	    removeBackupState( name );
	CCIter i=cont.begin();
	while( i!=cont.end() )
	    {
	    backups[name].push_back( (*i)->getCopy() );
	    ++i;
	    }
	}
    y2mil( "states:" << backupStates() );
    y2milestone( "ret:%d", ret );
    if( ret==0 )
	y2milestone( "comp:%d", equalBackupStates( name, "", true ));
    return( ret );
    }

int
Storage::removeBackupState( const string& name )
    {
    int ret = readonly?STORAGE_CHANGE_READONLY:0;
    assertInit();
    y2milestone( "name:%s", name.c_str() );
    if( ret==0 )
	{
	if( !name.empty() )
	    {
	    map<string,CCont>::iterator i = backups.find( name );
	    if( i!=backups.end())
		{
		deleteClist( i->second );
		backups.erase(i);
		}
	    else
		ret = STORAGE_BACKUP_STATE_NOT_FOUND;
	    }
	else
	    deleteBackups();
	}
    y2mil( "states:" << backupStates() );
    y2milestone( "ret:%d", ret );
    return( ret );
    }

int
Storage::restoreBackupState( const string& name )
    {
    int ret = readonly?STORAGE_CHANGE_READONLY:0;
    assertInit();
    y2milestone( "name:%s", name.c_str() );
    if( ret==0 )
	{
	map<string,CCont>::iterator b = backups.find( name );
	if( b!=backups.end())
	    {
	    cont.clear();
	    CCIter i=b->second.begin();
	    while( i!=b->second.end() )
		{
		cont.push_back( (*i)->getCopy() );
		++i;
		}
	    }
	else
	    ret = STORAGE_BACKUP_STATE_NOT_FOUND;
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

bool
Storage::checkBackupState( const string& name )
    {
    bool ret = false;
    assertInit();
    y2milestone( "name:%s", name.c_str() );
    map<string,CCont>::iterator i = backups.find( name );
    ret = i!=backups.end();
    y2milestone( "ret:%d", ret );
    return( ret );
    }

struct equal_co
    {
    equal_co( const Container* const co ) : c(co) {};
    bool operator()(const Container* co) { return( *co==*c ); }
    const Container* const c;
    };

bool
Storage::equalBackupStates( const string& lhs, const string& rhs,
                            bool verbose_log ) const
    {
    y2milestone( "lhs:%s rhs:%s verbose:%d", lhs.c_str(), rhs.c_str(),
                 verbose_log );
    map<string,CCont>::const_iterator i;
    const CCont* l = NULL;
    const CCont* r = NULL;
    if( lhs.empty() )
	l = &cont;
    else
	{
	i = backups.find( lhs );
	if( i!=backups.end() )
	    l = &i->second;
	}
    if( rhs.empty() )
	r = &cont;
    else
	{
	i = backups.find( rhs );
	if( i!=backups.end() )
	    r = &i->second;
	}
    bool ret = (l==NULL&&r==NULL) || (r!=NULL&&l!=NULL);
    if( ret && r!=NULL && l!=NULL )
	{
	CCIter i=l->begin();
	CCIter j;
	while( (ret||verbose_log) && i!=l->end() )
	    {
	    j = find_if( r->begin(), r->end(), equal_co( *i ) );
	    if( j!=r->end() )
		ret = (*i)->compareContainer( *j, verbose_log ) && ret;
	    else
		{
		ret = false;
		if( verbose_log )
		    y2mil( "container -->" << (**i) );
		}
	    ++i;
	    }
	i=r->begin();
	while( (ret||verbose_log) && i!=r->end() )
	    {
	    j = find_if( l->begin(), l->end(), equal_co( *i ) );
	    if( j==r->end() )
		{
		ret = false;
		if( verbose_log )
		    y2mil( "container <--" << (**i) );
		}
	    ++i;
	    }
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

string
Storage::backupStates() const
    {
    string ret;
    for( map<string,CCont>::const_iterator i = backups.begin();
         i!=backups.end(); ++i )
	{
	if( i!=backups.begin() )
	    ret += ',';
	ret += i->first;
	}
    return( ret );
    }


void
Storage::activateHld(bool val)
{
    y2mil("val:" << val);
    if (val)
    {
	Dm::activate(val);
	MdCo::activate(val, tmpDir());
    }
    LvmVg::activate(val);
    if (!val)
    {
	Dm::activate(val);
	MdCo::activate(val, tmpDir());
    }
}


void
Storage::activateMultipath(bool val)
{
    y2mil("val:" << val);
    DmmultipathCo::activate(val);
}


int Storage::addFstabEntry( const string& device, const string& mount,
                            const string& vfs, const string& options,
			    unsigned freq, unsigned passno )
    {
    int ret = readonly?STORAGE_CHANGE_READONLY:0;
    assertInit();
    y2milestone( "device:%s mount:%s vfs:%s opts:%s freq:%u passno:%u",
                 device.c_str(), mount.c_str(), vfs.c_str(), options.c_str(),
		 freq, passno );
    if( ret==0 && (device.empty()||mount.empty()||vfs.empty()))
	{
	ret = STORAGE_INVALID_FSTAB_VALUE;
	}
    if( ret==0 && fstab==NULL )
	{
	ret = STORAGE_NO_FSTAB_PTR;
	}
    if( ret==0 )
	{
	FstabChange c;
	c.device = c.dentry = device;
	c.mount = mount;
	c.fs = vfs;
	if( options.empty() )
	    c.opts.push_back( "defaults" );
	else
	    c.opts = splitString( options, "," );
	c.freq = freq;
	c.passno = passno;
	fstab->addEntry( c );
	if( isRootMounted() )
	    {
	    string dir = root() + mount;
	    if( access( dir.c_str(), R_OK )!=0 )
		createPath( dir );
	    ret = fstab->flush();
	    }
	}
    y2milestone( "ret:%d", ret );
    return( ret );
    }

void Storage::setExtError( const string& txt )
    {
    extendedError = txt;
    }


int Storage::waitForDevice() const
{
    int ret = 0;
    if (access(UDEVADM, X_OK) == 0)
    {
	string cmd(UDEVADM " settle --timeout=20");
	y2mil("calling prog:" << cmd);
	SystemCmd c(cmd);
	y2mil("returned prog:" << cmd << " retcode:" << c.retcode());
    }
    y2mil("ret:" << ret);
    return ret;
}


int Storage::waitForDevice( const string& device ) const
    {
    int ret = 0;
    waitForDevice();
    bool exist = access( device.c_str(), R_OK )==0;
    y2milestone( "device:%s exist:%d", device.c_str(), exist );
    if( !exist )
	{
	unsigned count=0;
	while( !exist && count<500 )
	    {
	    usleep( 10000 );
	    exist = access( device.c_str(), R_OK )==0;
	    count++;
	    }
	y2milestone( "device:%s exist:%d", device.c_str(), exist );
	}
    if( !exist )
	ret = STORAGE_DEVICE_NODE_NOT_FOUND;
    y2milestone( "ret:%d", ret );
    return( ret );
    }


void Storage::checkDeviceExclusive( const string& device, unsigned secs )
{
    const int delay = 50000;
    const unsigned count = secs * 1000000/delay;
    y2mil( "dev:" << device << " sec:" << secs << " count:" << count );
    for( unsigned i=0; i<count; i++ )
    {
	int fd = open( device.c_str(), O_RDONLY|O_EXCL );
	y2mil( "count:" << i << " fd:" << fd );
	if( fd>=0 )
	    close(fd);
	usleep( delay );
    }
}


int
Storage::zeroDevice(const string& device, unsigned long long sizeK, bool random,
		    unsigned long long startK, unsigned long long endK)
{
    y2mil("device:" << device << " sizeK:" << sizeK << " random:" << random <<
	  "startK:" << startK << " endK:" << endK);

    int ret = 0;

    const string source = (random ? "/dev/urandom" : "/dev/zero");

    SystemCmd c;
    string cmd;

    startK = min(startK, sizeK);
    cmd = DDBIN " if=" + source + " of=" + quote(device) + " bs=1k count=" + decString(startK);
    if (c.execute(cmd) != 0)
	ret = STORAGE_ZERO_DEVICE_FAILED;

    endK = min(endK, sizeK);
    cmd = DDBIN " if=" + source + " of=" + quote(device) + " seek=" + decString(sizeK - endK) +
	" bs=1k count=" + decString(endK);
    if (c.execute(cmd) != 0)
	ret = STORAGE_ZERO_DEVICE_FAILED;

    y2mil("ret:" << ret);
    return ret;
}


string Storage::byteToHumanString(unsigned long long size, bool classic, int precision, 
				  bool omit_zeroes) const
{
    return storage::byteToHumanString(size, classic, precision, omit_zeroes);
}


bool Storage::humanStringToByte(const string& str, bool classic, unsigned long long& size) const
{
    return storage::humanStringToByte(str, classic, size);
}


namespace storage
{
std::ostream& operator<< (std::ostream& s, Storage &v )
    {
    v.printInfo(s);
    return(s);
    }
}



Storage::SkipDeleted Storage::SkipDel;

storage::CallbackProgressBar Storage::progress_bar_cb_ycp;
storage::CallbackShowInstallInfo Storage::install_info_cb_ycp;
storage::CallbackInfoPopup Storage::info_popup_cb_ycp;
storage::CallbackYesNoPopup Storage::yesno_popup_cb_ycp;
