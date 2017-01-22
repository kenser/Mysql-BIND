/*
 * MySQL BIND SDB Driver
 *
 * Copyright (C) 2003-2004 Fairy Zhang <fairyling.zhang@gmail.com>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, 
 * MA 02111-1307 USA
 *
 * $Id:mysqldb.c,v1.0, 2013-4-11 Fairy Exp $ 
 */

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <mysql.h>

#include <isc/mem.h>
#include <isc/print.h>
#include <isc/result.h>
#include <isc/util.h>
#include <isc/buffer.h>
#include <isc/hash.h>

#include <dns/sdb.h>
#include <dns/result.h>

#include <dns/log.h>

#include <named/globals.h>

#include "include/mysqldb.h"

#include <time.h>
/*
 * This file is a modification of the PostGreSQL version which is distributed
 * in the contrib/sdb/pgsql/ directory of the BIND 9.2.2 source code,
 * modified by Fairy <fairyling.zhang@gmail.com>
 *
 * A simple database driver that interfaces to a MySQL database.  This
 * is not necessarily complete nor designed for general use, but it is has
 * been in use on production systems for some time without known problems.
 * It opens one connection to the database per zone, which is inefficient.  
 * It also may not handle quoting correctly.
 *
 * This driver is mainly designed for complex network in China. This driver will
 * response different rdata depending on not view but location informations 
 * (ISP, location and special IDC) of local DNS. However it's also support view function.
 *
 * The table must contain the fields "name", "rdtype",  "rdata", and location 
 * information ("isp", "location" and "idc"). It  is expected to contain a properly 
 * constructed zone.  
 *
 * Example SQL to create a domain
 * ==============================
 *
 * CREATE TABLE `mydomain` (
 * `id` int(11) NOT NULL auto_increment,
 * `name` varchar(255) NOT NULL default '',
 * `ttl` int(11) NOT NULL default '0',
 * `rdtype_id` int(11) NOT NULL default '0',
 * `rdata` varchar(255) NOT NULL default '',
 * `isp_id` int(11) NOT NULL default '0',
 * `location_id` int(11) NOT NULL default '0',
 * `idc_id` int(11) NOT NULL default '0',
 * `flag` tinyint(4) NOT NULL default '0',  
 * `opt_flag` int(11) NOT NULL default '0',
 * `sub_time` int(11) NOT NULL default '0',
 * `opt_time` int(11) NOT NULL default '0',
 * PRIMARY KEY  (`id`)
 * ) DEFAULT CHARSET=utf8;
 * 
 *
 * Example entry in named.conf
 * ===========================
 *
 * zone "mydomain.com" {
 *	type master;
 *	notify no;
 *	database "mysqldb dbname tablename hostname user password";
 * };
 *
 * Rebuilding the Server (modified from bind9/doc/misc/sdb)
 * =====================================================
 * 
 * The driver module and header file (mysqldb.c and mysqldb.h) 
 * must be copied to (or linked into)
 * the bind9/bin/named and bind9/bin/named/include directories
 * respectively, and must be added to the DBDRIVER_OBJS and DBDRIVER_SRCS
 * lines in bin/named/Makefile.in (e.g. add mysqldb.c to DBDRIVER_SRCS and 
 * mysqldb.@O@ to DBDRIVER_OBJS).  
 * 
 * Add the results from the command `mysql_config --cflags` to DBDRIVER_INCLUDES.
 * (e.g. DBDRIVER_INCLUDES = -I'/usr/include/mysql')
 *
 * Add the results from the command `mysql_config --libs` to DBRIVER_LIBS.
 * (e.g. DBDRIVER_LIBS = -L'/usr/lib/mysql' -lmysqlclient -lz -lcrypt -lnsl -lm -lc -lnss_files -lnss_dns -lresolv -lc -lnss_files -lnss_dns -lresolv)
 * 
 */

static dns_sdbimplementation_t *mysqldb = NULL;

struct ip_tbl_info{
	char zone[256];
	long source_ip;
	int isp_id;
	int location_id;
	int idc_id;
};

struct dbinfo
{
    MYSQL conn;
    char *database;
    char *table;
    char *host;
    char *user;
    char *passwd;
    struct ip_tbl_info zone_info;
};

struct data_info
{
	int fan_flag;
	int rdtype;
	char rdtype_str[sizeof("TYPE65536")];
	char rdata[250];
	int isp;
	int location;
	int idc;
	dns_ttl_t ttl;
};

typedef struct mysqldb_ipinfo
{
	isc_int16_t isp_id;
	isc_int16_t location_id;
	isc_int16_t idc_id;
}mysqldb_ipinfo_t;

typedef struct mysqldb_datanode mysqldb_datanode_t;
typedef struct mysqldb_datainfo mysqldb_datainfo_t;
typedef struct mysqldb_zonedata mysqldb_zonedata_t;

//structure of data location information 
struct mysqldb_datainfo{
	isc_int16_t isp;
	isc_int16_t location;
	isc_int16_t idc;
	char rdata[255];//data of resset
	dns_rdatatype_t rdtype; //type of domain 
	dns_ttl_t ttl;//ttl of domain
	
	mysqldb_datainfo_t *link;
};

//structure of mysqldb node
struct mysqldb_datanode{
	mysqldb_datanode_t *hashnext;//link of data nodes which are the same hash value ,used for hash colision
	
	char ndata[255];//string of domain name

	/*data location information including all the data of same domain order by the sequence of A, CNAME and NS for domain*/
	mysqldb_datainfo_t head;
	isc_uint16_t listcount;
};

//structure of mysqldb data
struct mysqldb_zonedata{
	unsigned int            nodecount;
	unsigned int            hashsize;
	mysqldb_datanode_t **        hashtable;
};

static void mysqldb_destroy(const char *zone, void *driverdata, void **dbdata, void *zone_data);

/*
 * Canonicalize a string before writing it to the database.
 * "dest" must be an array of at least size 2*strlen(source) + 1.
 */
static void quotestring(const char *source, char *dest)
{
    while (*source != 0)
    {
        if (*source == '\'')
            *dest++ = '\'';
        /* SQL doesn't treat \ as special, but PostgreSQL does */
        else if (*source == '\\')
            *dest++ = '\\';
        *dest++ = *source++;
    }
    *dest++ = 0;
}

/*
 * Connect to the database.
 */
static isc_result_t db_connect(struct dbinfo *dbi)
{
    if(!mysql_init(&dbi->conn))
	    return (ISC_R_MYSQLDBNOTCONNECT);
	
    if (mysql_real_connect(&dbi->conn, dbi->host, dbi->user, dbi->passwd, dbi->database, 0, NULL, 0))
        return (ISC_R_SUCCESS);
    else
 	    return (ISC_R_MYSQLDBNOTCONNECT);
}

/*
 * Check to see if the connection is still valid.  If not, attempt to
 * reconnect.
 */
static isc_result_t maybe_reconnect(struct dbinfo *dbi)
{
    if (!mysql_ping(&dbi->conn))
        return (ISC_R_SUCCESS);

    return (db_connect(dbi));
}

#define ANY 0
#define TOP_RULE 4
#define interconn_isp 8

static int mysqldb_findrule_res(const char *domain_name,const char *fan_name,
								struct data_info *data_info,const int row,
								const int rule_id, const int isp_id,const int location_id, const int idc_id,
								const dns_rdatatype_t type,dns_sdblookup_t *lookup){
	if(rule_id < -1)
		return 0;
	if(row<=1)
		return 0;
	if(type<0)
		return 0;
	
	int i;
	int count = 0;
	int flag;
	int soa_flag = 0;
	int cname_flag = 0;
	int fan_flag = 0;
	int a_flag = 0;
	char *data;
	isc_result_t result = 0;

	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(7),
			      "%s:source data:rule_id %d idc_id %d isp_id %d location_id %d,type %d",
			      __func__,rule_id,idc_id,isp_id,location_id,type);
			     
	for(i=0;i<row;i++){
		flag = 0;

		if(data_info[i].fan_flag == 0 &&
			(type == dns_rdatatype_a || type == dns_rdatatype_aaaa) && 
			(data_info[i].rdtype == type || data_info[i].rdtype == dns_rdatatype_cname)){
			a_flag = 1;
		}
		
		if(rule_id == 4){//same idc or ANY
			if((data_info[i].idc==idc_id ||data_info[i].idc == ANY ) 
				&& data_info[i].fan_flag == 0)
				flag = 1;
			
		}else if(rule_id == 3){//same isp and location (special. interconn_isp)
			if((data_info[i].isp == isp_id || data_info[i].isp == ANY 
					||(data_info[i].isp == interconn_isp && (isp_id ==1 ||isp_id ==2))) 
				&&(data_info[i].location == location_id || data_info[i].location == ANY)
				&& data_info[i].fan_flag == 0)
				
				flag = 1;
			
		}else if(rule_id == 2){//same isp (special. interconn_isp)
			if((data_info[i].isp == isp_id || data_info[i].isp == ANY 
					||(data_info[i].isp == interconn_isp && (isp_id ==1 ||isp_id ==2))) 
				&& data_info[i].fan_flag == 0)
				
				flag = 1;
			
		}else if(rule_id == 1){//same location
			if((data_info[i].location == location_id || data_info[i].location == ANY)
				&& data_info[i].fan_flag == 0)
				
				flag = 1;
			
		}else if(rule_id == 0){
			if(data_info[i].fan_flag == 0)
				flag = 1;
		}else if(rule_id == -1){
			if(data_info[i].fan_flag == 1)
				flag = 1;
		}

		if(flag == 1){//same type
			flag = 0;
			if(type == dns_rdatatype_any || type == 0)
				flag = 1;
			else if(type == dns_rdatatype_a || type == dns_rdatatype_aaaa){
				if(data_info[i].rdtype ==  type || data_info[i].rdtype == dns_rdatatype_cname || (a_flag == 0 && data_info[i].rdtype == dns_rdatatype_ns))
					flag = 1;
			}else if(data_info[i].rdtype ==  type)
				flag =1;
		}

		if(flag == 1){
			if((soa_flag == 0 && cname_flag ==0) 
				|| (soa_flag == 1 && data_info[i].rdtype != dns_rdatatype_soa)
				|| (cname_flag == 1 && data_info[i].rdtype != dns_rdatatype_cname && data_info[i].rdtype != dns_rdatatype_ns)){
				
		 		result = dns_sdb_putrr(lookup, data_info[i].rdtype_str, data_info[i].ttl, &data_info[i].rdata);
			
			    if (result != ISC_R_SUCCESS)
			        return 0;
			 	
				count ++;
			}
			
			if(data_info[i].rdtype == dns_rdatatype_soa)
				soa_flag = 1;
			if(data_info[i].rdtype == dns_rdatatype_a || data_info[i].rdtype == dns_rdatatype_aaaa)
				cname_flag = 1;
			if(data_info[i].rdtype == dns_rdatatype_cname)
				cname_flag = 1;
		}
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(7),
			      "%s:data info:flag %d,type %d %s,isp %d,location %d,idc %d,data %s",
			      __func__,flag,data_info[i].rdtype,data_info[i].rdtype_str,data_info[i].isp,data_info[i].location,data_info[i].idc,data_info[i].rdata);
	}

	return count;
}

static MYSQL_RES *mysqldb_return_res(MYSQL *conn, const char *domain_name, const char *table_name, 
									      const dns_rdatatype_t type,const char *zone){
	if (conn == NULL)
		return 0;
	int i = 0;
	MYSQL_RES *res=0;
	char str[1500];
	char rdtype_where[100];
	char fan_domain[50] = "*.";
	
	memset(str, '\0', sizeof(str));
	memset(rdtype_where,'\0',sizeof(rdtype_where));
	
	if(type>0 && type!=dns_rdatatype_any){
		if(type == dns_rdatatype_a){
			snprintf(rdtype_where,sizeof(rdtype_where)," and (rdtype_id=%d or rdtype_id=%d or rdtype_id=%d)  order by find_in_set(rdtype_id,'%d,%d,%d')",type,dns_rdatatype_cname,dns_rdatatype_ns,type,dns_rdatatype_cname,dns_rdatatype_ns);
		}else{
			snprintf(rdtype_where,sizeof(rdtype_where)," and rdtype_id=%d ",type);
		}
	}
	
	snprintf(str, sizeof(str),"SELECT ttl, rdata,rdtype_id,isp_id,location_id,idc_id,name,rdtype FROM `%s` WHERE flag=1 and (name = '%s' or name='*.%s') %s", table_name, domain_name,zone,rdtype_where);
	if( mysql_query(conn, str) != 0 ) {
        isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			      "There is an error when query '%s' in '%s'",
			      str,__func__);
	    memset(str, '\0', sizeof(str));
	    return 0;
    }

	res = mysql_store_result(conn);

   if (mysql_num_rows(res) == 0){
		mysql_free_result(res);
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(3),
			      "There is no records when query '%s' in '%s'",
			      str,__func__);
		memset(str, '\0', sizeof(str));
		return 0;
   }
		
	memset(str, '\0', sizeof(str));
	return res;
}


static int mysqldb_put_res(MYSQL *conn,const char* domain_name,const *table_name,
							const dns_rdatatype_t type,const char *zone, dns_sdblookup_t *lookup,
							mysqldb_ipinfo_t *ip_info,struct ip_tbl_info *ip_tbl_info){
	if (conn == NULL)
		return 0;
	int i = 0;
	MYSQL_RES *res=0;
	int rule = TOP_RULE;
	int row = 0;
	MYSQL_ROW tmp = 0;
	isc_result_t result;
	int row2_fan_flag = 0;
	char fan_domain[50];

	strncpy(fan_domain,"*.",50);
	
	strncat(fan_domain,zone,50);
		
	//find records which are valid
	res = mysqldb_return_res(conn,domain_name,table_name,type,zone);
	if(res == 0)
		return (ISC_R_NOTFOUND);

	row = mysql_num_rows(res);
	if(row <= 0)
		return (ISC_R_NOTFOUND);

	char *ttlstr;
	char *data;
	char *type_str;
	char *isp_str;
	char *location_str;
	char *idc_str;
	isc_buffer_t buf;
	dns_ttl_t ttl;
	int type_id;
	int isp;
	int location;
	int idc;
	char *domain;
	char *rdtype_str;
	
	struct data_info *data_info;
		
	if(row>1){
		data_info = isc_mem_get(ns_g_mctx, row*sizeof(struct data_info));
	    if (data_info == NULL){
			isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
					      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
					      "There is no memery for Mysql driver putting res.");
	        return (ISC_R_NOMEMORY);

	    }
	}
		
    while(tmp = mysql_fetch_row(res)){
		ttlstr = tmp[0];
	    data   = tmp[1];
		type_str = tmp[2];
		isp_str = tmp[3];
		location_str = tmp[4];
		idc_str = tmp[5];
		domain = tmp[6];
		rdtype_str = tmp[7];
		char *endp;
	
	    ttl = strtol(ttlstr, &endp, 10);
		type_id = strtol(type_str, &endp, 10);

		isp = strtol(isp_str, &endp, 10);
		location = strtol(location_str, &endp, 10);
		idc = strtol(idc_str, &endp, 10);
		
		if (*endp != '\0'){
	    	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
				      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
				      "There is a error of TLL '%s' or TYPE_ID '%s'",
				      ttlstr,type_str);

			if(row>1)
				isc_mem_put(ns_g_mctx, data,row*sizeof(struct data_info));
				mysql_free_result(res);
		    	return (DNS_R_BADTTL);
	    }

		if(row == 1){//if thers is only 1 record ,return it
			result = dns_sdb_putrr(lookup, rdtype_str, ttl, data);
			mysql_free_result(res);
				
		    if (result != ISC_R_SUCCESS) {
		        return (ISC_R_FAILURE);
		 	}
			return (ISC_R_SUCCESS);
		}
			
		data_info[i].rdtype = type_id;
		data_info[i].isp = isp;
		data_info[i].location = location;
		data_info[i].idc = idc;
		data_info[i].ttl = ttl;
		strncpy(data_info[i].rdata,data,250); 
		strncpy(data_info[i].rdtype_str,rdtype_str,sizeof("TYPE65536"));
		
		if(strncmp(domain,fan_domain, 255) == 0){
			data_info[i].fan_flag = 1;
			if(row == 2)
				row2_fan_flag = 1;
		}else
			data_info[i].fan_flag = 0;
			
		i++;
	}
	mysql_free_result(res);

	if(row2_fan_flag == 1){
		for(i=0;i<2;i++){
			if(data_info[i].fan_flag == 0){
				result = dns_sdb_putrr(lookup, data_info[i].rdtype_str, data_info[i].ttl, data_info[i].rdata);
				isc_mem_put(ns_g_mctx, data_info,2*sizeof(struct data_info));
			    	if (result != ISC_R_SUCCESS) {
			             return (ISC_R_FAILURE);
			 	}
				return (ISC_R_SUCCESS);
			}
		}
	}
		
	int isp_id = -1;
	int location_id = -1;
	int idc_id = -1;

	if(ip_info){
		isp_id = ip_info->isp_id;
		location_id = ip_info->location_id;
		idc_id = ip_info->idc_id;
	}

	if(idc_id <= 0){
		rule = 3;
		if(isp_id<=0){
			rule = 1;
			if(location_id <= 0)
				rule = 0;
		}else if(location_id <=0){
			rule = 2;
			if(isp_id <= 0)
				rule = 0;

		}
	}

	int ret_rule = 0;
	for(i=rule;i>=-1;i--){
		ret_rule = mysqldb_findrule_res(domain_name,fan_domain,data_info,row,i,isp_id,location_id,idc_id,type,lookup);
		if (ret_rule !=0){
			if(row>1)
				isc_mem_put(ns_g_mctx, data_info,row*sizeof(struct data_info));
				
			if(i!=rule && (type == dns_rdatatype_a ||type == dns_rdatatype_aaaa)){
				isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
					      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(1),
					      "Rule %d is suit for domain %s when source IP (isp %d,location %d,idc %d) is looking for rdtype %d",
					      i,domain_name,isp_id,location_id,idc_id,type);
			}
				
			return (ISC_R_SUCCESS);
		}
			
		ret_rule = 0;
	}

	if(row>1)
		isc_mem_put(ns_g_mctx, data_info,row*sizeof(struct data_info));
	return (ISC_R_NOTFOUND);
}

static unsigned int return_hashval(const char *domain){
	return isc_hash_calc(domain, strlen(domain), ISC_FALSE);
}

static isc_result_t mysqldb_return_typestr(dns_rdatatype_t type, char *typestr){
	isc_buffer_t * b = NULL;
	isc_result_t result;
	isc_region_t r;

	memset(typestr, '\0', sizeof("TYPE65536"));
	result = isc_buffer_allocate(ns_g_mctx, &b, 4000);
	if(result != ISC_R_SUCCESS)
		return result;

	result = dns_rdatatype_totext(type, b);
	if(result != ISC_R_SUCCESS)
		return result;
	
	isc_buffer_usedregion(b, &r);
	r.base[r.length] = '\0';
	strncpy(typestr,r.base, sizeof("TYPE65536"));
	
	isc_buffer_free(&b);
	return (ISC_R_SUCCESS);
}

/*
 * finding rdata for returning from datalist
 *
 * args 	-	datanode : hash list pointer of hash table which ndata is the same as query name
 *			   rule_id : rule number (4 - same idc, 3 - same isp and location, 2 - same isp, 1 - same location, 0 - all).
 *			     isp_id : isp number of source ip
 *		     location_id : location number of source ip
 *			     idc_id : idc number of source ip
 *				type : type id of user request (eg. A, CNAME and so on)
 *			    lookup : pointer of SDB driver for returning lookup data
 *
 * return value - zero : not find res set or error
 *		   non-zero : the number of returning data count
 */
static isc_result_t mysqldb_findrule_datalist(mysqldb_datanode_t *datanode, const int rule_id,
										const isc_int16_t isp_id, const isc_int16_t location_id, const isc_int16_t idc_id,
										const dns_rdatatype_t type, dns_sdblookup_t *lookup){
	
	if(datanode == NULL)
		return 0;
	if(rule_id < -1)
		return 0;
	if(type<0)
		return 0;
	
	mysqldb_datainfo_t *datalist = &datanode->head;
	if(datalist == NULL)
		return 0;
	
	int flag;
	int soa_flag = 0;
	int cname_flag = 0;
	int a_flag = 0;
	int count = 0;
	
	char *ndata = datanode->ndata;
	dns_ttl_t ttl = 0;
	char rdtype_str[sizeof("TYPE65536")];	
	int ttl_count = 0;
	
	isc_result_t result;

	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(7),
			      "%s:source data:rule_id %d idc_id %d isp_id %d location_id %d,type %d",
			      __func__,rule_id,idc_id,isp_id,location_id,type);
			     
	for(;datalist != NULL;datalist = datalist->link){
		flag = 0;
		if(type == dns_rdatatype_a && 
			(datalist->rdtype == type || datalist->rdtype == dns_rdatatype_cname)){
			a_flag = 1;
		}
		
		if(rule_id == 4){//same idc or ANY
			if(datalist->idc== idc_id ||datalist->idc == ANY )
				flag = 1;
			
		}else if(rule_id == 3){//same isp and location (special. interconn_isp)
			if((datalist->isp == isp_id || datalist->isp == ANY 
					||(datalist->isp == interconn_isp && (isp_id ==1 ||isp_id ==2))) 
				&&(datalist->location == location_id || datalist->location == ANY))
				flag = 1;
			
		}else if(rule_id == 2){//same isp (special. interconn_isp)
			if(datalist->isp == isp_id || datalist->isp == ANY 
					||(datalist->isp == interconn_isp && (isp_id ==1 ||isp_id ==2)))
				flag = 1;
			
		}else if(rule_id == 1){//same location
			if(datalist->location == location_id || datalist->location == ANY)
				flag = 1;
			
		}else if(rule_id == 0){
				flag = 1;
		}

		if(flag == 1){//same type
			flag = 0;
			if(type == dns_rdatatype_any || type == 0)
				flag = 1;
			else if(type == dns_rdatatype_a){
				if(datalist->rdtype ==  type || datalist->rdtype == dns_rdatatype_cname || (a_flag == 0 && datalist->rdtype == dns_rdatatype_ns))
					flag = 1;
			}else if(datalist->rdtype ==  type)
				flag =1;
		}

		if(flag == 1){
			if((soa_flag == 0 && cname_flag ==0) 
				|| (soa_flag == 1 && datalist->rdtype != dns_rdatatype_soa)
				|| (cname_flag == 1 && datalist->rdtype != dns_rdatatype_cname && datalist->rdtype != dns_rdatatype_ns)){

				if(count == 0)
					ttl = datalist->ttl;
				
				result = mysqldb_return_typestr(datalist->rdtype, rdtype_str);
				if(result == ISC_R_SUCCESS){
			 		result = dns_sdb_putrr(lookup, rdtype_str, ttl, datalist->rdata);
				
				    if (result != ISC_R_SUCCESS) {
				        return result;
				 	}
					count++;
				}else{
					isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
						      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
						      "%s:data info:flag %d,type eror (%d),isp %d,location %d,idc %d,data %s",
						      __func__,flag,datalist->rdtype,datalist->isp,datalist->location,datalist->idc,datalist->rdata);
				}
			}
			
			if(datalist->rdtype == dns_rdatatype_soa)
				soa_flag = 1;
			if(datalist->rdtype == dns_rdatatype_a || datalist->rdtype == dns_rdatatype_aaaa)
				cname_flag = 1;
			if(datalist->rdtype == dns_rdatatype_cname)
				cname_flag = 1;
		}

		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(7),
			      "%s:data info:flag %d,type %d,isp %d,location %d,idc %d,data %s",
			      __func__,flag,datalist->rdtype,datalist->isp,datalist->location,datalist->idc,datalist->rdata);
	}

	return count;
}

/*
 * finding rdata for returning from datalist
 *
 * args 		datanode : hash list pointer of hash table which ndata is the same as query name
 *			    ip_info : ip information structure pointer of source ip
 *				type : type id of user request (eg. A, CNAME and so on)
 *			    lookup : pointer of SDB driver for returning lookup data
 *
 * return - result : the value of ISC_R_SUCCESS is meaning found successfully.
 *			      the value of ISC_R_FAILURE and other value is meaning error .
 */
static isc_result_t mysqldb_find_datalist(mysqldb_datanode_t *datanode, 
									const mysqldb_ipinfo_t *ip_info,
									const dns_rdatatype_t type, dns_sdblookup_t *lookup){
	if(datanode == NULL || lookup == NULL)
		return (ISC_R_FAILURE);

	if(datanode->listcount == 0)
		return (ISC_R_FAILURE);

	if(datanode->listcount == 1){
		mysqldb_datainfo_t *datalist = &datanode->head;
		char rdtype_str[sizeof("TYPE65536")];
		isc_result_t result;
		
		if(datalist == NULL)
			return (ISC_R_FAILURE);
		if(type == datalist->rdtype || 
			(type == dns_rdatatype_a && (datalist->rdtype==dns_rdatatype_cname || datalist->rdtype==dns_rdatatype_ns))){
			result = mysqldb_return_typestr(datalist->rdtype, rdtype_str);
			if(result == ISC_R_SUCCESS){
		 		result = dns_sdb_putrr(lookup, rdtype_str,datalist->ttl, datalist->rdata);
				return result;
			}else{
				isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
					      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
					      "%s:data info:type eror (%d),isp %d,location %d,idc %d,data %s",
					      __func__,datalist->rdtype,datalist->isp,datalist->location,datalist->idc,datalist->rdata);
			}
		}

		return (ISC_R_FAILURE);
	}

	isc_result_t result;
	int rule = 4;
	int i = 0;
	int isp_id = -1;
	int location_id = -1;
	int idc_id = -1;

	if(ip_info){
		isp_id = ip_info->isp_id;
		location_id = ip_info->location_id;
		idc_id = ip_info->idc_id;
	}
	
	// finding start rule number for user request
	if(idc_id <= 0){
		rule = 3;
		if(isp_id<=0){
			rule = 1;
			if(location_id <= 0)
				rule = 0;
		}else if(location_id <=0){
			rule = 2;
			if(isp_id <= 0)
				rule = 0;

		}
	}
	//find result order by rule rollback
	int ret_rule = 0;
	for(i=rule;i>=0;i--){
		ret_rule = mysqldb_findrule_datalist(datanode, i, isp_id, location_id, idc_id, type, lookup);
		if (ret_rule !=0){
			if(i!=rule && (type == dns_rdatatype_a ||type == dns_rdatatype_aaaa)){
				isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
				      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(1),
				      "Rule %d is suit for domain %s when source IP (isp %d,location %d,idc %d) is looking for rdtype %d",
				      i,datanode->ndata,isp_id,location_id,idc_id,type);
			}
			return (ISC_R_SUCCESS);
		}
		ret_rule = 0;
	}
	return (ISC_R_FAILURE);
}

/*
 * finding datanode from hash table
 *
 * args 	domain_name : domain name of user request
 *		      zone_data : hash list pointer of hash table which ndata is the same as query name
 *
 * return - result : the value of ISC_R_SUCCESS is meaning found successfully.
 *			      the value of ISC_R_FAILURE and other value is meaning error .
 */
static mysqldb_datanode_t *mysqldb_find_node(const char *domain_name, mysqldb_zonedata_t *zone_data){
	
	mysqldb_datanode_t *datanode;
	unsigned int hashval;
	int i;
	
	if(domain_name == NULL || zone_data == NULL)
		return NULL;
	
	hashval = return_hashval(domain_name)%zone_data->hashsize;
	if(hashval <0)
		return NULL;
	datanode = zone_data->hashtable[hashval];
	if(datanode == 0)
		return NULL;

	//find data list from hash list
	for(;datanode!=0;datanode=datanode->hashnext){
		if(strncasecmp(datanode->ndata, domain_name, 255) == 0)
			break;
	}
	
	return datanode;//not found if null
}

/*
 * finding rdata for returning from datalist
 *
 * args 			zone : zone name of user request
 *		domain_name : domain name of user request
 *			    lookup : pointer of SDB driver for returning lookup data
 *			    ip_info : ip information structure pointer of source ip
 *				type : type id of user request (eg. A, CNAME and so on)
 * 		      zone_data : hash list pointer of hash table which ndata is the same as query name
 *
 * return - result : the value of ISC_R_SUCCESS is meaning found successfully.
 *			      the value of ISC_R_FAILURE and other value is meaning error .
 */
static isc_result_t mysqldb_find_res(const char *zone, const char *domain_name, 
	                           					dns_sdblookup_t *lookup, const mysqldb_ipinfo_t *ip_info, 
	                           					const dns_rdatatype_t type, mysqldb_zonedata_t *zone_data){
	isc_result_t result;
	mysqldb_datanode_t *datanode;
	
	if(zone == NULL || domain_name == NULL ||lookup == NULL || zone_data == NULL)
		return (ISC_R_FAILURE);
	
	datanode = mysqldb_find_node(domain_name,zone_data);
	if(datanode == NULL){
		//find *.zone
		char fan_name[255];
		memset(fan_name,'\0',255);
		snprintf(fan_name,sizeof(fan_name),"*.%s",zone);
		
		datanode = mysqldb_find_node(fan_name,zone_data);
		if(datanode == NULL)
			return (ISC_R_FAILURE);
	}

	result = mysqldb_find_datalist(datanode, ip_info, type, lookup);
	return result;
}

const char *
gettime(void) {
	struct timeval tv;
	static char now[512];

	gettimeofday(&tv, NULL);

	snprintf(now, sizeof(now) - 1,
		 "%d %d",tv.tv_sec,tv.tv_usec);

	return (now);
}
/*
 * This database operates on absolute names.
 *
 * Queries are converted into SQL queries and issued synchronously.  Errors
 * are handled really badly.
 */
static isc_result_t mysqldb_lookup(const char *zone, const char *name, void *dbdata,
	                           	dns_sdblookup_t *lookup, dns_clientinfomethods_t *methods,
		       		dns_clientinfo_t *clientinfo,void *ip_info,dns_rdatatype_t type, void *zone_data){
	isc_result_t result;
	struct dbinfo *dbi = dbdata;
	mysqldb_ipinfo_t *mysqldb_ipinfo = ip_info;
	mysqldb_zonedata_t *mysqldb_zonedata = zone_data;

	UNUSED(methods);
	UNUSED(clientinfo);
	
	int ret_value = 0;
	if(zone_data == NULL){
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(5),
			      "Mysql driver lookup() Checking for db connecting: '%s' '%s'",
			      dbi->database,dbi->table);
		
		result = maybe_reconnect(dbi);
		if (result != ISC_R_SUCCESS){
				isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
				      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
				      "Mysql driver lookup() Cannot connect to database: '%s' '%s' ",
				      dbi->database,dbi->table);
				return result;
		}

		ret_value = mysqldb_put_res(&dbi->conn,name,dbi->table,type,zone,lookup,mysqldb_ipinfo,&dbi->zone_info);
	}else{
		ret_value = mysqldb_find_res(zone,name,lookup, mysqldb_ipinfo, type, mysqldb_zonedata);
	}
	return ret_value;
	
}

/*
 * Issue an SQL query to return all nodes in the database and fill the
 * allnodes structure.
 */
static isc_result_t mysqldb_allnodes(const char *zone, void *dbdata, dns_sdballnodes_t *allnodes)
{
    struct dbinfo *dbi = dbdata;
    isc_result_t result;
    MYSQL_RES *res = 0;
    MYSQL_ROW row;
    char str[1500];

    UNUSED(zone);
    snprintf(str, sizeof(str),
	 "SELECT ttl, name, rdtype, rdata FROM `%s` WHERE flag=1 ORDER BY name",
	 dbi->table);

    result = maybe_reconnect(dbi);
    if (result != ISC_R_SUCCESS){
	    isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			      "Mysql driver allnodes() Cannot connect to database: '%s' '%s'",
			      dbi->database,dbi->table);
	    return result;

    }

    if( mysql_query(&dbi->conn, str) != 0 ){
    	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			      "There is an error when query '%s' for Mysql driver allnodes() ",
			      str);
    	return (ISC_R_FAILURE);
    }
    res = mysql_store_result(&dbi->conn);

    if (mysql_num_rows(res) == 0){
    	mysql_free_result(res);
    	return (ISC_R_NOTFOUND);
    }

    while ((row = mysql_fetch_row(res))) {
        char *ttlstr = row[0];
	    char *name   = row[1];
    	char *type   = row[2];
    	char *data   = row[3];
	    dns_ttl_t ttl;
	    char *endp;
	    ttl = strtol(ttlstr, &endp, 10);
	    if (*endp != '\0') {
           mysql_free_result(res);
           return (DNS_R_BADTTL);
	    }
	    result = dns_sdb_putnamedrr(allnodes, name, type, ttl, data);
	    if (result != ISC_R_SUCCESS){
           mysql_free_result(res);
           return (ISC_R_FAILURE);
	    }
    }
    mysql_free_result(res);
    return (ISC_R_SUCCESS);
}

/*
 * Create a connection to the database and save any necessary information
 * in dbdata.
 *
 * argv[0] is the name of the database
 * argv[1] is the name of the table
 * argv[2] (if present) is the name of the host to connect to
 * argv[3] (if present) is the name of the user to connect as
 * argv[4] (if present) is the name of the password to connect with
 */
static isc_result_t mysqldb_create(const char *zone, int argc, char **argv,
	                           void *driverdata, void **dbdata)
{
    struct dbinfo *dbi;
    isc_result_t result;

    UNUSED(zone);
    UNUSED(driverdata);

    if (argc < 2){
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			      "Mysql driver requires more than 2 args.");
		return (ISC_R_FAILURE);
    }
    dbi = isc_mem_get(ns_g_mctx, sizeof(struct dbinfo));
    if (dbi == NULL){
	    isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			      "There is no memery for Mysql driver creation.");
        return (ISC_R_NOMEMORY);

    }
        
    dbi->database = NULL;
    dbi->table    = NULL;
    dbi->host     = NULL;
    dbi->user     = NULL;
    dbi->passwd   = NULL;

    dbi->zone_info.source_ip = 0;
    dbi->zone_info.idc_id = 0;
    dbi->zone_info.isp_id = 0;
    dbi->zone_info.location_id = 0;
	
    memcpy(dbi->zone_info.zone,"\0",sizeof(dbi->zone_info.zone));

#define STRDUP_OR_FAIL(target, source)			\
    do                                                  \
    {							\
	target = isc_mem_strdup(ns_g_mctx, source);	\
	if (target == NULL)                             \
        {				                \
            result = ISC_R_NOMEMORY;		        \
	    goto cleanup;				\
	}						\
    } while (0);

    STRDUP_OR_FAIL(dbi->database, argv[0]);
    STRDUP_OR_FAIL(dbi->table,    argv[1]);
    STRDUP_OR_FAIL(dbi->host,     argv[2]);
    STRDUP_OR_FAIL(dbi->user,     argv[3]);
    STRDUP_OR_FAIL(dbi->passwd,   argv[4]);

    result = db_connect(dbi);
    if (result != ISC_R_SUCCESS)
	    goto cleanup;

    *dbdata = dbi;
    return (ISC_R_SUCCESS);

cleanup:
    mysqldb_destroy(zone, driverdata, (void **)&dbi, NULL);
    return result;
}

static void mysqldb_zonedata_destroy(mysqldb_zonedata_t *zone_data){
	if(zone_data == NULL)
		return ;
	
	int hashval = 0;
	mysqldb_datanode_t *datanode;
	mysqldb_datanode_t *nodenext;
	mysqldb_datainfo_t *datainfo;
	mysqldb_datainfo_t *infolink;
	int nodecount = 0;
	isc_mem_t *mctx = ns_g_mctx;

	for(hashval = 0; hashval<zone_data->hashsize; hashval++){
		datanode = zone_data->hashtable[hashval];
		
		if(datanode!= 0){
			while(datanode){
				nodenext = datanode->hashnext;
				
				datainfo = datanode->head.link;
				while(datainfo){
					infolink = datainfo->link;
					isc_mem_put(mctx, datainfo, sizeof(mysqldb_datainfo_t));
					datainfo = infolink;
					nodecount ++;
				}

				isc_mem_put(mctx, datanode, sizeof(mysqldb_datanode_t));
				datanode = nodenext;
				nodecount ++;
			}
		}
	}
	isc_mem_put(mctx, zone_data->hashtable, zone_data->hashsize*sizeof(mysqldb_datanode_t *));
	isc_mem_put(mctx, zone_data, sizeof(mysqldb_zonedata_t));
}
/*
 * Close the connection to the database.
 */
static void mysqldb_destroy(const char *zone, void *driverdata, void **dbdata, void *zone_data){
	struct dbinfo *dbi = *dbdata;
	mysqldb_zonedata_t *mysqldb_zone_data = zone_data;
	UNUSED(zone);
	UNUSED(driverdata);

	if(dbi){
		mysql_close(&dbi->conn);
		if (dbi->database != NULL)
		    isc_mem_free(ns_g_mctx, dbi->database);
		if (dbi->table != NULL)
		    isc_mem_free(ns_g_mctx, dbi->table);
		if (dbi->host != NULL)
		    isc_mem_free(ns_g_mctx, dbi->host);
		if (dbi->user != NULL)
		    isc_mem_free(ns_g_mctx, dbi->user);
		if (dbi->passwd != NULL)
		    isc_mem_free(ns_g_mctx, dbi->passwd);

		isc_mem_put(ns_g_mctx, dbi, sizeof(struct dbinfo));
	}
	mysqldb_zonedata_destroy(mysqldb_zone_data);
	
}

static isc_result_t mysqldb_zdata(const char *zone, void *driverdata, void **m_zone_data);

static isc_result_t mysqldb_zdupdate(const char *zone, const char *domain, void *driverdata, void **m_zone_data);

/*
 * Since the SQL database corresponds to a zone, the authority data should
 * be returned by the lookup() function.  Therefore the authority() function
 * is NULL.
 */
static dns_sdbmethods_t mysqldb_methods = {
    mysqldb_lookup,
    NULL, /* authority */
    mysqldb_allnodes,
    mysqldb_create,
    mysqldb_destroy,
    NULL,/* lookup2 */
    mysqldb_zdata,
    mysqldb_zdupdate 
};

/*
 * Wrapper around dns_sdb_register().
 */
isc_result_t mysqldb_init(void)
{
    unsigned int flags;
    flags = 0;
    isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
		      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(2),
		      "Registering MYSQL  driver.");
    return (dns_sdb_register("mysqldb", &mysqldb_methods, NULL, flags,
            ns_g_mctx, &mysqldb));
}

/*
 * Wrapper around dns_sdb_unregister().
 */
void mysqldb_clear(void)
{
	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
		      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(2),
		      "Registering MYSQL  driver.");
    if (mysqldb != NULL)
        dns_sdb_unregister(&mysqldb);
}

typedef isc_result_t (*mysqldb_addfunc_t)(void *, const char *, const dns_ttl_t , const dns_rdataclass_t ,const isc_int16_t , const isc_int16_t , const isc_int16_t , const char *);


#define MYSQLDB_HASH_SIZE           64
#define LIST_TAIL(node,link) \
	while(node->link){node = node->link;} 
	
static isc_result_t trans_zone_into_dbname(MYSQL *conn, const char *zone, char *tbl_name);
static int split_qname_by_dot(const char *qname, char **split_name);
static isc_result_t mysqldb_default_dbinfo_init(isc_mem_t *mctx, struct dbinfo *dbi) ;
static isc_result_t mysqldb_zonedata_create(isc_mem_t *mctx, mysqldb_zonedata_t **zonedata);
static isc_result_t insert_to_hashtable(isc_mem_t *mctx, mysqldb_datanode_t **hashtbl, unsigned int hashval,
									const char *domain, const dns_ttl_t ttl, const dns_rdataclass_t rdtype,
									const isc_int16_t isp, const isc_int16_t location, const isc_int16_t idc,
									const char *rdata);
static isc_result_t mysqldb_add_node(void *m_zonedata,
									const char *domain, const dns_ttl_t ttl, const dns_rdataclass_t rdtype,
									const isc_int16_t isp, const isc_int16_t location, const isc_int16_t idc,
									const char *rdata);

static MYSQL_RES *mysqldb_return_zonedata_res(MYSQL *conn, const char *tbl_name, char *wherestr);
static isc_result_t insert_tbldata_intohashtbl(MYSQL_RES *res, void *zone_data, mysqldb_addfunc_t action);

static isc_result_t mysqldb_datanode_create(isc_mem_t *mctx, mysqldb_datanode_t **data);
static isc_result_t mysqldb_add_nodeanddata(void *m_datanode,
									const char *domain, const dns_ttl_t ttl, const dns_rdataclass_t rdtype,
									const isc_int16_t isp, const isc_int16_t location, const isc_int16_t idc,
									const char *rdata);

/*
  * Destroy database information structure
  */
static void dbi_destroy(isc_mem_t *mctx ,struct dbinfo *dbi){
	isc_result_t result;
	if(dbi == NULL)
		return ;

	isc_mem_put(mctx,dbi->database,30*sizeof(char));
	isc_mem_put(mctx,dbi->host,30*sizeof(char));
	isc_mem_put(mctx,dbi->user,30*sizeof(char));
	isc_mem_put(mctx,dbi->passwd,30*sizeof(char));

	isc_mem_put(mctx,dbi,sizeof(struct dbinfo));
}

/*
  * Initialize database information
  */
static isc_result_t dbi_init(void *driverdata, struct dbinfo **dbi){
	isc_result_t result;
	
	if(driverdata != NULL){
		*dbi = driverdata;
		return (ISC_R_SUCCESS);
	}

	*dbi = isc_mem_get(ns_g_mctx, sizeof(struct dbinfo));
	if(*dbi == NULL)
		return (ISC_R_NOMEMORY);

	result = mysqldb_default_dbinfo_init(ns_g_mctx, *dbi);
	if(result != ISC_R_SUCCESS){
		isc_mem_put(ns_g_mctx,*dbi,sizeof(struct dbinfo));
		return result;
	}
	result = db_connect(*dbi);
	
	if(result != ISC_R_SUCCESS){
		dbi_destroy(ns_g_mctx, *dbi);
		return result;
	}
	return (ISC_R_ADDITIONALMEM);
}


/*Caching zone data for Intelligent DNS using Hash Table structure.
 * Mounting the data to SDB->zone_data
 *
 * args  -    zone : zone name of SDB friver
 *	   driverdata : Database connection information in configure file
 *	   zone_data : the address of SDB->zone_data for returning
 *
 * return - result : the value of ISC_R_SUCCESS is meaning cacheing successfully.
 *			      the value of ISC_R_FAILURE is meaning error or table not existing.
 */
static isc_result_t mysqldb_zdata(const char *zone, void *driverdata, void **m_zone_data) {
	struct dbinfo *dbi;
	mysqldb_zonedata_t *zone_data;
	isc_result_t result;
	char tbl_name[255];
	MYSQL_RES *res=0;
	int row;
	int get_dbi_flag = 0;
	
	/*
	  * Make the Hash Table structure.
	  */
	result = mysqldb_zonedata_create(ns_g_mctx, &zone_data);
	if(result != ISC_R_SUCCESS)
		return result;
	
	*m_zone_data = zone_data;
	
	result = dbi_init(driverdata, &dbi);
	if(result == ISC_R_ADDITIONALMEM)
		get_dbi_flag = 1;
	else if(result != ISC_R_SUCCESS)
		return result;
	result = maybe_reconnect(dbi);
	if (result != ISC_R_SUCCESS){
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			      "Cannot connect to database '%s' when finding zone data (%s).",
			      dbi->database, zone);
		if(get_dbi_flag == 1)
			dbi_destroy(ns_g_mctx, dbi);
		return result;
	}

	if(trans_zone_into_dbname(&dbi->conn, zone, tbl_name) == ISC_R_FAILURE){
		if(get_dbi_flag == 1)
			dbi_destroy(ns_g_mctx, dbi);
		return (ISC_R_FAILURE);
	}

	res = mysqldb_return_zonedata_res(&dbi->conn, tbl_name, NULL);
	if(res == 0){
		if(get_dbi_flag == 1)
			dbi_destroy(ns_g_mctx, dbi);
		return (ISC_R_NOTFOUND);
	}
	row = mysql_num_rows(res);
	if(row <= 0){
		if(get_dbi_flag == 1)
			dbi_destroy(ns_g_mctx, dbi);
		return (ISC_R_NOTFOUND);
	}

	result = insert_tbldata_intohashtbl(res, zone_data, mysqldb_add_node);
	if(get_dbi_flag == 1){
		dbi_destroy(ns_g_mctx, dbi);
	}
	mysql_free_result(res);

	return result;
}

static isc_result_t insert_tbldata_intohashtbl(MYSQL_RES *res, void *zone_data, mysqldb_addfunc_t action){
	if(res == NULL || zone_data == NULL)
		return (ISC_R_FAILURE);
	
	char *ttlstr;
	char *rdata;
	char *type_str;
	char *isp_str;
	char *location_str;
	char *idc_str;
	isc_buffer_t buf;
	dns_ttl_t ttl;
	dns_rdataclass_t rdtype;
	isc_int16_t isp;
	isc_int16_t location;
	isc_int16_t idc;
	char *domain;
	MYSQL_ROW tmp = 0;
	isc_result_t result;
	
	while(tmp = mysql_fetch_row(res)){
		domain = tmp[0];
		ttlstr = tmp[1];
	    rdata   = tmp[2];
		type_str = tmp[3];
		isp_str = tmp[4];
		location_str = tmp[5];
		idc_str = tmp[6];
		
		char *endp;
		
	    ttl = strtol(ttlstr, &endp, 10);
		rdtype = strtol(type_str, &endp, 10);

		isp = strtol(isp_str, &endp, 10);
		location = strtol(location_str, &endp, 10);
		idc = strtol(idc_str, &endp, 10);
		
		if (*endp != '\0'){
	    	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			      "There is a error of TLL '%s' or TYPE_ID '%s'",
			      ttlstr,type_str);
			mysql_free_result(res);
	    	return (DNS_R_BADTTL);
	    }
		
		result = action(zone_data, domain, ttl, rdtype, isp, location, idc, rdata);
		if(result != ISC_R_SUCCESS)
			return result;
		
	}
	return (ISC_R_SUCCESS);
}

/* Updating the caching data of domain 'name'.
 *
 * args  -    zone : zone name of SDB friver
 *		   name : domain name which want to be modified
 *	   driverdata : Database connection information in configure file
 *	   zone_data : SDB->zone_data
 *
 * return - result : the value of ISC_R_SUCCESS is meaning cacheing successfully.
 *			      the value of ISC_R_FAILURE is meaning error or table not existing.
 */
static isc_result_t mysqldb_zdupdate(const char *zone, const char *domain, void *driverdata, void **m_zone_data){
	struct dbinfo *dbi;
	mysqldb_zonedata_t *zone_data = *m_zone_data;
	isc_result_t result;
	mysqldb_datanode_t *datanode;
	int get_dbi_flag = 0;
	MYSQL_RES *res=0;
	char tbl_name[255];
	char wherestr[1500];
	int row;

	if(zone == NULL || domain == NULL)
		return (ISC_R_FAILURE);

/*
 * Reload informations for all domain
 */
	if(*domain == '\0'){
		mysqldb_zonedata_t *new_zone_data = NULL;
		mysqldb_zonedata_t *old_zone_data = NULL;
		old_zone_data = zone_data;
		result = mysqldb_zdata(zone, driverdata, &new_zone_data);
		if(result == ISC_R_SUCCESS){
			*m_zone_data = new_zone_data;
			mysqldb_zonedata_destroy(old_zone_data);
		}else{
			mysqldb_zonedata_destroy(new_zone_data);
		}
		return result;
	} 

	if(zone_data == NULL){
		return (ISC_R_FAILURE);
	}

/*
 * Reload information for specializing domain
 */

/*
  * Initialize database information
  */
	result = dbi_init(driverdata, &dbi);
	if(result == ISC_R_ADDITIONALMEM)
		get_dbi_flag = 1;
	else if(result != ISC_R_SUCCESS)
		return result;
	
	result = maybe_reconnect(dbi);
	if (result != ISC_R_SUCCESS){
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			      "Cannot connect to database '%s' when finding zone data (%s).",
			      dbi->database, zone);
		if(get_dbi_flag == 1)
			dbi_destroy(ns_g_mctx, dbi);
		return result;
	}
	

/*
  * Find the table name of zone
  */
	if(trans_zone_into_dbname(&dbi->conn, zone, tbl_name) == ISC_R_FAILURE){
		if(get_dbi_flag == 1)
			dbi_destroy(ns_g_mctx, dbi);
		return (ISC_R_FAILURE);
	}
	
	snprintf(wherestr, sizeof(wherestr), " AND name='%s'", domain);
	res = mysqldb_return_zonedata_res(&dbi->conn, tbl_name, wherestr);
	if(res == 0){
		if(get_dbi_flag == 1)
			dbi_destroy(ns_g_mctx, dbi);
		return (ISC_R_NOTFOUND);
	}
	row = mysql_num_rows(res);
	if(row <= 0){
		if(get_dbi_flag == 1)
			dbi_destroy(ns_g_mctx, dbi);
		return (ISC_R_NOTFOUND);
	}

	/*
	  * Malloc datanode memory
	 */
	result = mysqldb_datanode_create(ns_g_mctx, &datanode);
	if(result != ISC_R_SUCCESS)
		return result;

	result = insert_tbldata_intohashtbl(res, datanode, mysqldb_add_nodeanddata);
	if(result == ISC_R_SUCCESS){
		/*
		 * replace 
		 */
		int hashval = 0;
		mysqldb_datanode_t *orig_datanode, *tmp_datanode;
		int find_datanode = 0;

		hashval = return_hashval(domain)%zone_data->hashsize;
		orig_datanode = zone_data->hashtable[hashval];
	
		if(orig_datanode == 0){
			zone_data->hashtable[hashval] = datanode;
			zone_data->nodecount += datanode->listcount;
		}else{
			tmp_datanode = orig_datanode;
			
			for(;orig_datanode!=0;orig_datanode=orig_datanode->hashnext){
				if(tmp_datanode == orig_datanode){
					zone_data->hashtable[hashval] = datanode;
					datanode->hashnext = orig_datanode->hashnext;
					orig_datanode->hashnext = NULL;
					find_datanode = 1;
					break;
				}
				if(strncasecmp(orig_datanode->ndata, domain, 255) == 0){
					tmp_datanode->hashnext = datanode;
					datanode->hashnext = orig_datanode->hashnext;
					orig_datanode->hashnext = NULL;
					find_datanode = 1;
					break;
				}
				tmp_datanode = orig_datanode;
			}
			
			if(find_datanode == 1){//release orig_datanode
				mysqldb_datainfo_t *datainfo;
				mysqldb_datainfo_t *infolink;
				
				zone_data->nodecount += datanode->listcount;
				zone_data->nodecount -= orig_datanode->listcount;
				
				if(orig_datanode){
					datainfo = orig_datanode->head.link;
					while(datainfo){
						infolink = datainfo->link;
						isc_mem_put(ns_g_mctx, datainfo, sizeof(mysqldb_datainfo_t));
						datainfo = NULL;
						datainfo = infolink;
					}
					isc_mem_put(ns_g_mctx, orig_datanode, sizeof(mysqldb_datanode_t));
					orig_datanode = NULL;	
				}
				
			}else{
				tmp_datanode->hashnext = datanode;
				zone_data->nodecount += datanode->listcount;
			}
		}
	}
	if(get_dbi_flag == 1){
		dbi_destroy(ns_g_mctx, dbi);
	}
	mysql_free_result(res);
	return result;
}

static isc_result_t mysqldb_datainfo_create(isc_mem_t *mctx, mysqldb_datainfo_t **info){
	*info = isc_mem_get(mctx, sizeof(mysqldb_datainfo_t));
	if(*info == NULL )
		return (ISC_R_NOMEMORY);

	(*info)->idc = 0;
	(*info)->isp = 0;
	(*info)->location = 0;
	(*info)->ttl = 0;
	(*info)->link = NULL;
	memset((*info)->rdata,'\0',sizeof((*info)->rdata));
	return (ISC_R_SUCCESS);
}

static isc_result_t mysqldb_datanode_create(isc_mem_t *mctx, mysqldb_datanode_t **data){
	*data = isc_mem_get(mctx, sizeof(mysqldb_datanode_t));
	if(*data == NULL )
		return (ISC_R_NOMEMORY);

	memset((*data)->ndata,'\0',sizeof((*data)->ndata));
	(*data)->hashnext = NULL;
	(*data)->listcount = 0;
	
	return (ISC_R_SUCCESS);
}

static isc_result_t mysqldb_datainfo_init(mysqldb_datainfo_t *info,const dns_rdataclass_t rdtype,
										const isc_int16_t isp_id, const isc_int16_t location_id, const isc_int16_t idc_id,
										const dns_ttl_t ttl, const char *rdata){
	if(info == NULL || rdata == NULL)
		return (ISC_R_FAILURE);

	info->rdtype = rdtype;
	info->isp = isp_id;
	info->location = location_id;
	info->idc = idc_id;
	info->ttl = ttl;
	strncpy(info->rdata, rdata, sizeof(info->rdata));
	info->link = NULL;
	
	return (ISC_R_SUCCESS);
}

static isc_result_t mysqldb_datanode_init(mysqldb_datanode_t *data,
										const char *domain, const dns_ttl_t ttl, const dns_rdataclass_t rdtype,
										const isc_int16_t isp_id, const isc_int16_t location_id, const isc_int16_t idc_id,
										const char *rdata){
	if(data == NULL || domain == NULL )
		return (ISC_R_FAILURE);

	isc_result_t result;

	strncpy(data->ndata, domain, 255);
	data->hashnext = NULL;
	result = mysqldb_datainfo_init(&data->head,rdtype,isp_id,location_id,idc_id, ttl, rdata);

	if(result == ISC_R_SUCCESS)
		data->listcount = 1;
	return result;
}

static isc_result_t insert_into_datalist(mysqldb_datainfo_t *dst,mysqldb_datainfo_t *src){
	isc_result_t result;
	mysqldb_datainfo_t *info;
	mysqldb_datainfo_t *dstinfo = dst;
	int insert_flag = 0;

	if(dst == NULL || src == NULL)
		return (ISC_R_FAILURE);

	for(info=dstinfo;info->link!=NULL;info = info->link){
		if(src->rdtype == dns_rdatatype_a){//A record, insert first
			src->link = info->link;
			info->link = src;
			return (ISC_R_SUCCESS);
			
		}else if (src->rdtype == dns_rdatatype_cname){//CNAME record  insert it after A
			if(info->link->rdtype != dns_rdatatype_a){
				src->link = info->link;
				info->link = src;
				return (ISC_R_SUCCESS);
			}
		}
	}

	info->link = src;
	return (ISC_R_SUCCESS);
}

/*Add data node for Intelligent DNS to Hash Table structure.
 * 1. create data node if hash table is null 
 * 2. insert into data list if the same hash table is not null
 * 3. create new data node if the same hash table is null
 *
 * args  -  	mctx : memory list of BIND
 *		    hashtbl : the first address of hash table
 *		   hashval : hash value of domain
 *		    domain : domain name
 *	    		    ttl : TTL
 *	   	     rdtype : the data type of this data (eg. A , SOA, CNAME ...)
 *			   isp : isp id 
 *		    location: location id
 *			   idc : idc id
 *
 * return - result : the value of ISC_R_SUCCESS is meaning add successfully.
 *			      the value of ISC_R_FAILURE and other value is meaning error .
 */
static isc_result_t insert_to_hashtable(isc_mem_t *mctx, mysqldb_datanode_t **hashtbl, unsigned int hashval,
									const char *domain, const dns_ttl_t ttl, const dns_rdataclass_t rdtype,
									const isc_int16_t isp, const isc_int16_t location, const isc_int16_t idc,
									const char *rdata){
	isc_result_t result;
	mysqldb_datanode_t *datanode = NULL;
	mysqldb_datanode_t *tmp_datanode = NULL;
	mysqldb_datainfo_t *datainfo = NULL;

	if(hashtbl == NULL || domain == NULL)
		return (ISC_R_FAILURE);
	if(hashval <0)
		return (ISC_R_FAILURE);
	if(hashval >= MYSQLDB_HASH_SIZE)
		hashval = hashval % MYSQLDB_HASH_SIZE;
	
	if(hashtbl[hashval] == 0){//hashtbl is null,create a datanode and its head
		result = mysqldb_datanode_create(mctx, &hashtbl[hashval]);
		if(result != ISC_R_SUCCESS)
			return result;

		result = mysqldb_datanode_init(hashtbl[hashval], domain, ttl, rdtype, isp,location,idc, rdata);
		if(result != ISC_R_SUCCESS)
			return result;
	}else{
		tmp_datanode = hashtbl[hashval];
		int find_flag = 0;
		while(tmp_datanode != NULL){//find the datanode whose ndata is same as domain
			if(strncasecmp(domain, tmp_datanode->ndata, 255) == 0){//insert data into this data list
				result = mysqldb_datainfo_create(mctx, &datainfo);
				if(result != ISC_R_SUCCESS)
					return result;
				result = mysqldb_datainfo_init(datainfo, rdtype, isp, location, idc, ttl, rdata);
				if(result != ISC_R_SUCCESS)
					return result;

				result = insert_into_datalist(&tmp_datanode->head, datainfo);
				if(result != ISC_R_SUCCESS)
					return result;
				tmp_datanode->listcount++;
				find_flag = 1;
				break;
			}
			tmp_datanode = tmp_datanode->hashnext;
		}
		if(find_flag == 0){// insert data into tail of hashnext data list
			result = mysqldb_datanode_create(mctx, &datanode);
			if(result != ISC_R_SUCCESS)
				return result;
			result = mysqldb_datanode_init(datanode, domain, ttl, rdtype, isp,location,idc, rdata);
			if(result != ISC_R_SUCCESS)
				return result;
			tmp_datanode = hashtbl[hashval];
			LIST_TAIL(tmp_datanode, hashnext);
			tmp_datanode->hashnext = datanode;
		}
	}

	return result;
}

/*Add zone data for Intelligent DNS to Hash Table structure.
 * 
 * args -m_zonedata : zone data pointer to SDB driver
 *		    domain : domain name
 *	    		    ttl : TTL
 *	   	     rdtype : the data type of this data (eg. A , SOA, CNAME ...)
 *			   isp : isp id 
 *		    location: location id
 *			   idc : idc id
 *
 * return - result : the value of ISC_R_SUCCESS is meaning add successfully.
 *			      the value of ISC_R_FAILURE is meaning error .
 */
static isc_result_t mysqldb_add_node(void *m_zonedata,
									const char *domain, const dns_ttl_t ttl, const dns_rdataclass_t rdtype,
									const isc_int16_t isp, const isc_int16_t location, const isc_int16_t idc,
									const char *rdata){
	if(m_zonedata == NULL)
		return (ISC_R_FAILURE);
	if(domain == NULL)
		return (ISC_R_FAILURE);

	mysqldb_zonedata_t *zonedata = m_zonedata;
	unsigned int hashval;
	isc_result_t result;

	hashval = return_hashval(domain)%zonedata->hashsize;

	if(hashval <0 || hashval>=zonedata->hashsize)
		return ISC_R_FAILURE;
	
	result = insert_to_hashtable(ns_g_mctx,zonedata->hashtable, hashval, domain, ttl, rdtype, isp, location,idc,rdata);

	if(result == ISC_R_SUCCESS)
		zonedata->nodecount ++;

	return result;
}

static isc_result_t mysqldb_add_nodeanddata(void *m_datanode,
									const char *domain, const dns_ttl_t ttl, const dns_rdataclass_t rdtype,
									const isc_int16_t isp, const isc_int16_t location, const isc_int16_t idc,
									const char *rdata){
		if(m_datanode == NULL)
			return (ISC_R_FAILURE);
		if(domain == NULL)
			return (ISC_R_FAILURE);
	
		mysqldb_datanode_t *datanode = m_datanode;
		isc_result_t result;
		if(datanode->listcount == 0){
			mysqldb_datanode_init(datanode, domain, ttl, rdtype, isp, location, idc, rdata);
		}else{
			mysqldb_datainfo_t *datainfo;
			
			result = mysqldb_datainfo_create(ns_g_mctx, &datainfo);
			if(result != ISC_R_SUCCESS)
				return result;
			result = mysqldb_datainfo_init(datainfo, rdtype, isp, location, idc, ttl, rdata);
			if(result != ISC_R_SUCCESS)
				return result;

			result = insert_into_datalist(&datanode->head, datainfo);
			if(result != ISC_R_SUCCESS)
				return result;
			datanode->listcount++;
		}

		return (ISC_R_SUCCESS);

}

/*Create zone data structure.
 *
 * args  -    mctx : link of memory for BIND memory manager
 *	   zone_data : the zonedata structure for returning   
 *
 * return - result : the value of ISC_R_SUCCESS is meaning creating successfully.
 *			      the value of ISC_R_FAILURE is meaning error.
 */
static isc_result_t mysqldb_zonedata_create(isc_mem_t *mctx, mysqldb_zonedata_t **zonedata)
{
	unsigned int bytes;
	
	*zonedata = isc_mem_get(mctx, sizeof(mysqldb_zonedata_t));
	if(*zonedata == NULL)
		return (ISC_R_NOMEMORY);

	(*zonedata)->hashsize = MYSQLDB_HASH_SIZE;
	(*zonedata)->nodecount = 0;

	bytes = (*zonedata)->hashsize * sizeof(mysqldb_datanode_t *);
	(*zonedata)->hashtable = isc_mem_get(mctx, bytes);
	
	if((*zonedata)->hashtable == NULL){
		isc_mem_put(mctx, *zonedata, sizeof(*zonedata));
		*zonedata = NULL;
		return (ISC_R_NOMEMORY);
	}

	memset((*zonedata)->hashtable, 0, bytes);
	
	return (ISC_R_SUCCESS);
}

/*
  * Mysql Database default connection information
*/
#define DB_NAME	"mysql_bind"
#define DB_HOST "localhost"
#define DB_USER "root"
#define DB_PASSWD ""

/*
 * Initialize default datase information by defination
*/
static isc_result_t mysqldb_default_dbinfo_init(isc_mem_t *mctx, struct dbinfo *dbi) {

	if(mctx == NULL || dbi == NULL)
		return (ISC_R_FAILURE);
	
	dbi->database = isc_mem_get(mctx, 30*sizeof(char));
	if(dbi->database == NULL)
		return (ISC_R_NOMEMORY);
	
	dbi->host = isc_mem_get(mctx, 30*sizeof(char));
	if(dbi->host == NULL){
		isc_mem_put(mctx,dbi->database,30*sizeof(char));
		return (ISC_R_NOMEMORY);
	}

	dbi->user = isc_mem_get(mctx, 30*sizeof(char));
	if(dbi->user == NULL){
		isc_mem_put(mctx,dbi->database,30*sizeof(char));
		isc_mem_put(mctx,dbi->host,30*sizeof(char));
		return (ISC_R_NOMEMORY);
	}

	dbi->passwd = isc_mem_get(mctx, 30*sizeof(char));
	if(dbi->passwd == NULL){
		isc_mem_put(mctx,dbi->database,30*sizeof(char));
		isc_mem_put(mctx,dbi->host,30*sizeof(char));
		isc_mem_put(mctx,dbi->user,30*sizeof(char));
		return (ISC_R_NOMEMORY);
	}

	strncpy(dbi->database, DB_NAME,30);
	strncpy(dbi->host, DB_HOST,30);
	strncpy(dbi->user, DB_USER,30);
	strncpy(dbi->passwd, DB_PASSWD,30);

	return (ISC_R_SUCCESS);
}

/* tranlate zone name into DB table name
 *
 * args  -    conn : connection of IP Database
 *		 qname : Domain Name for resolving
 *
 * return - result : the value of ISC_R_SUCCESS is meaning successful.
*/
static isc_result_t trans_zone_into_dbname(MYSQL *conn, const char *zone, char *tbl_name){
	if(conn == NULL || zone == NULL || tbl_name == NULL)
		return (ISC_R_FAILURE);

	char *array[142];

	int loop = 0;
	int i = 0;
	char *tmp_tbl = tbl_name;
	
	loop = split_qname_by_dot(zone,array);

	if(loop == 0)
		return (ISC_R_FAILURE);
	
	for(i=0;i<loop;i++){
		snprintf(tmp_tbl, 255,"%s", array[i]);
		tmp_tbl += strlen(array[i]);
		if(i != loop-1){
			memcpy(tmp_tbl,"_",1);
			tmp_tbl += 1;
		}
	}

	return (ISC_R_SUCCESS);
}

/* split query name into arrays by '.' which are components of DB table name
 *
 * args  - qname : Domain Name to be splited
 *      split_name : name array which has been splited for returning
 *
 * return - spliting num : the value of non-zero is meaning spliting successfully.
 *					 the value of zero is meaning error or invalid query name.
*/
static int split_qname_by_dot(const char *qname, char **split_name){
	if(qname == NULL || split_name == NULL)
		return 0;

	int loop = 0;
	char name[255];
	char mdot[] = ".";

	strncpy(name, qname, 255);

	// split query name into arrays by '.' which are components of DB table name  
	split_name[0]=strtok(name, mdot);

	if(split_name[0]==NULL)
	       return 0;

	for(loop=1;loop<142;loop++){
	       split_name[loop]=strtok(NULL,mdot);
	       if(split_name[loop]==NULL)
	               break;
	}

	if(loop < 2)
		return 0;
	return loop;
}

/* return all the data set of tbl_name for zonedata fuction
 *
 * args  -    conn : connection for zonedata function
 *	    tbl_name : table name of zone
 *
 * return - result : the value of non-zero is meaning finding dataset successfully.
 *			      the value of zero is meaning not found.
*/
static MYSQL_RES *mysqldb_return_zonedata_res(MYSQL *conn, const char *tbl_name, char *wherestr){
	if (conn == NULL)
			return 0;
	
	MYSQL_RES *res=0;
	char str[1500];
	
	memset(str, '\0', sizeof(str));

	snprintf(str, sizeof(str),"SELECT name,ttl, rdata,rdtype_id,isp_id,location_id,idc_id FROM `%s` WHERE flag=1 order  by name", tbl_name);
	if(wherestr != NULL)
		strcat(str, wherestr);
	
	if( mysql_query(conn, str) != 0 ){
        isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			  DNS_LOGMODULE_MYSQL, ISC_LOG_ERROR,
			  "There is an error when query '%s' in '%s'",
			  str,__func__);
	     memset(str, '\0', sizeof(str));
	     return 0;
    }

	res = mysql_store_result(conn);

    if (mysql_num_rows(res) <= 0){
		mysql_free_result(res);
		res = 0;
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_MYSQL, ISC_LOG_DEBUG(3),
			      "There is no records when query '%s' in '%s'",
			      str,__func__);
		
		memset(str, '\0', sizeof(str));
		return 0;
    }
	
	memset(str, '\0', sizeof(str));
	return res;
}

