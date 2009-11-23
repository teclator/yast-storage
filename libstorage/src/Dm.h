/*
 * Copyright (c) [2004-2009] Novell, Inc.
 *
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, contact Novell, Inc.
 *
 * To contact Novell about this file by physical or electronic mail, you may
 * find current contact information at www.novell.com.
 */


#ifndef DM_H
#define DM_H

#include <map>

#include "y2storage/Volume.h"

namespace storage
{

class PeContainer;

class Dm : public Volume
    {
    public:
	Dm( const PeContainer& d, const string& tn );
	Dm( const PeContainer& d, const string& tn, unsigned mnum );
	Dm( const PeContainer& d, const Dm& rhs );
	Dm& operator=( const Dm& );

	virtual ~Dm();
	const string& getTableName() const { return( tname ); }
	const string& getTargetName() const { return( target ); }
	void setTableName( const string& name ) { tname=name; }
	bool inactive() const { return( inactiv ); }
	unsigned long long getLe() const { return num_le; }
	void setLe( unsigned long long le );
	void modifyPeSize( unsigned long long old, unsigned long long neww );
	bool removeTable();
	virtual void calcSize();
	const std::map<string,unsigned long>& getPeMap() const { return( pe_map ); }
	void setPeMap( const std::map<string,unsigned long>& m );
	unsigned long long usingPe( const string& dev ) const;
	bool mapsTo( const string& dev ) const;
	void getTableInfo();
	virtual bool checkConsistency() const;
	unsigned stripes() const { return stripe; }
	unsigned setStripes( unsigned long val ) { return stripe=val; }
	unsigned long long stripeSize() const { return stripe_size; }
	void setStripeSize( unsigned long long val ) { stripe_size=val; }
	string sysfsPath() const;
	void updateMajorMinor();

	virtual const std::list<string> udevId() const { return Volume::udevId(); }
	friend std::ostream& operator<< (std::ostream& s, const Dm &p );
	virtual void print( std::ostream& s ) const { s << *this; }
	virtual string removeText( bool doing ) const;
	virtual string formatText( bool doing ) const;
	void getInfo( storage::DmInfo& info ) const;
	void changeDeviceName( const string& old, const string& nw );

	static bool notDeleted( const Dm& l ) { return( !l.deleted() ); }

	static void activate( bool val=true );
	static bool isActive() { return active; }

	static string devToTable( const string& dev );
	static string dmName( const string& table );
	static int dmNumber( const string& table );
	bool equalContent( const Dm& rhs ) const;
	void logDifference( const Dm& d ) const;
	string stringDifference( const Dm& d ) const;
	static unsigned dmMajor();
	static string dmDeviceName( unsigned long num );

    protected:
	void init();
	const PeContainer* pec() const;
	virtual const string shortPrintedName() const { return( "Dm" ); }
	string getDevice( const string& majmin );
	static void getDmMajor();

	string tname;
	string target;
	unsigned long long num_le;
	unsigned stripe;
	unsigned long long stripe_size;
	bool inactiv;
	std::map<string,unsigned long> pe_map;
	static bool active;
	static unsigned dm_major;
	static std::list<string> known_types;
	mutable storage::DmInfo info;
    };

}

#endif