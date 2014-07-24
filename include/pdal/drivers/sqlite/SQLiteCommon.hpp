/******************************************************************************
* Copyright (c) 2012, Howard Butler, hobu.inc@gmail.com
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#pragma once

#include <pdal/pdal_error.hpp>
#include <pdal/Options.hpp>

#include <sqlite3.h>
#include <sstream>

namespace pdal
{
namespace drivers
{
namespace sqlite
{


    class sqlite_driver_error : public pdal_error
    {
    public:
        sqlite_driver_error(std::string const& msg)
            : pdal_error(msg)
        {}
    };

    class connection_failed : public sqlite_driver_error
    {
    public:
        connection_failed(std::string const& msg)
            : sqlite_driver_error(msg)
        {}
    };

    class buffer_too_small : public sqlite_driver_error
    {
    public:
        buffer_too_small(std::string const& msg)
            : sqlite_driver_error(msg)
        {}
    };

class column
{
public:
    
    column() : null(true), blobBuf(0), blobLen(0){};
    template<typename T> column( T v) : null(false), blobBuf(0), blobLen(0)
    {
        data = boost::lexical_cast<std::string>(v);
    }
    column(std::string v) : null(false), blobBuf(0), blobLen(0)
    {
        data = v;
    }

    std::string data;
    bool null;
    char * blobBuf;
    std::size_t blobLen;
};

class blob : public column
{
public:
    blob(char* buffer, std::size_t size) : column()
    {
        blobBuf = buffer;
        blobLen = size;
        
    }
};

typedef std::vector<column> row;
typedef std::vector<row> records;

class SQLite
{
public:
    SQLite(std::string const& connection, LogPtr log) 
        : m_log(log)
        , m_connection(connection)
        , m_session(0)
        , m_statement(0)
        , m_position(0)
    {
        sqlite3_config(SQLITE_CONFIG_LOG, log_callback, this);
    }
    
    ~SQLite()
    {
        if (m_statement)
        {
            sqlite3_finalize(m_statement);

        }
        
        if (m_session)
            sqlite3_close_v2(m_session);
    }
    void log(int num, char const* msg)
    {
        std::ostringstream oss;
        oss << "SQLite code: " << num << " msg: '" << msg << "'";
        m_log->get(logDEBUG) << oss.str() << std::endl;
    }

    static void log_callback(void *p, int num, char const* msg)
    {
        SQLite* sql = reinterpret_cast<SQLite*>(p);
        sql->log(num, msg);
    }
    
        
    void connect(bool bWrite=false)
    {
        if ( ! m_connection.size() )
        {
            throw connection_failed("unable to connect to sqlite3 database, no connection string was given!");
        }
        
        int flags = SQLITE_OPEN_NOMUTEX;
        if (bWrite) flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        
        int code = sqlite3_open_v2(m_connection.c_str(), &m_session, flags, 0);
        if ( (code != SQLITE_OK) ) 
        {
            check_error("unable to connect to database");
        }
        spatialite();
    }
    
    void execute(std::string const& sql, std::string errmsg="")
    {
        if (!m_session)
            throw sqlite_driver_error("Session not opened!");
        
        int code = sqlite3_exec(m_session, sql.c_str(), NULL, NULL, NULL);
        if (code != SQLITE_OK)
        {
            std::ostringstream oss;
            oss << errmsg <<" '" << sql << "'";
            throw sqlite_driver_error(oss.str());
        }

  
        
    }
    
    void begin()
    {
        execute("BEGIN", "Unable to begin transaction");
    }
    
    void commit()
    {
        execute("COMMIT", "Unable to commit transaction");
    }
    
    void query(std::string const& query)
    {
        m_position = 0;
        m_columns.clear();
        m_data.clear();
        sqlite3_reset(m_statement);
        
        char const* tail = 0; // unused;
        int res = sqlite3_prepare_v2(m_session,
                                  query.c_str(),
                                  static_cast<int>(query.size()),
                                  &m_statement,
                                  &tail);
        if (res != SQLITE_OK)
        {
            char const* zErrMsg = sqlite3_errmsg(m_session);

              std::ostringstream ss;
              ss << "sqlite3_statement_backend::prepare: "
                 << zErrMsg;
              throw sqlite_driver_error(ss.str());
        }
        int numCols = -1;

        while (res != SQLITE_DONE)
        {
            res = sqlite3_step(m_statement);

            if (SQLITE_ROW == res)
            {
                // only need to set the number of columns once
                if (-1 == numCols)
                {
                    numCols = sqlite3_column_count(m_statement);
                }
                
                row r;
                for (int v = 0; v < numCols; ++v)
                {
                    if (m_columns.size() != static_cast<std::vector<std::string>::size_type > (numCols))
                    {
                        const char* ccolumnName = sqlite3_column_name(m_statement, v);
                        m_columns.push_back(std::string(ccolumnName));
                    }
                    
                    column c;
                    char const* buf =
                        reinterpret_cast<char const*>(sqlite3_column_text(m_statement, v));
                    c.null = false;
                    if (0 == buf)
                    {
                        c.null = true;
                        buf = "";
                    }
                    c.data = buf;
                    r.push_back(c);
                }
                m_data.push_back(r);
            }
        }        
    }
    
    bool next()
    {
        m_position++;
        
        if (m_data.size() - 1 == m_position)
            return false;
        return true;
    }
    
    row* get()
    {
        return &m_data[m_position];
    }
    
    std::vector<std::string> const& columns() const
    {
        return m_columns;
    }
    
    int64_t last_row_id() const
    {
        return (int64_t)sqlite3_last_insert_rowid(m_session);
    }
    
    bool insert(std::string const& statement, records const& rs)
    {
        sqlite3_reset(m_statement);
        records::size_type rows = rs.size();

        int res = sqlite3_prepare_v2(m_session,
                                  statement.c_str(),
                                  static_cast<int>(statement.size()),
                                  &m_statement,
                                  0);
        if (res != SQLITE_OK)
        {
            char const* zErrMsg = sqlite3_errmsg(m_session);

              std::ostringstream ss;
              ss << "sqlite insert prepare: "
                 << zErrMsg;
              throw sqlite_driver_error(ss.str());
        }

        for (records::size_type r = 0; r < rows; ++r)
        {
            int const totalPositions = static_cast<int>(rs[0].size());
            for (int pos = 0; pos <= totalPositions-1; ++pos)
            {
                int didBind = SQLITE_OK;
                const column& c = rs[r][pos];
                if (c.null)
                {
                    didBind = sqlite3_bind_null(m_statement, pos+1);
                }
                else if (c.blobBuf)
                {
                    didBind = sqlite3_bind_blob(m_statement, pos+1,
                                                c.blobBuf,
                                                static_cast<int>(c.blobLen),
                                                SQLITE_STATIC);
                }
                else
                {
                    didBind = sqlite3_bind_text(m_statement, pos+1,
                                                c.data.c_str(),
                                                static_cast<int>(c.data.length()),
                                                SQLITE_STATIC);
                }

                if (SQLITE_OK != didBind)
                {
                    std::ostringstream oss;
                    oss << "Failure to bind row number '" 
                        << r <<"' at position number '" <<pos <<"'";
                    throw sqlite_driver_error(oss.str());
                }
            }
            
            int const res = sqlite3_step(m_statement);

            if (SQLITE_DONE == res)
            {
            }
            else if (SQLITE_ROW == res)
            {
            }
            else
            {
                char const* zErrMsg = sqlite3_errmsg(m_session);

                std::ostringstream ss;
                ss << "sqlite insert failure: "
                   << zErrMsg;
                throw sqlite_driver_error(ss.str());                
            }
        }
        
        return true;
    }
    
private:  
    pdal::LogPtr m_log;
    std::string m_connection;
    sqlite3* m_session;
    sqlite3_stmt* m_statement;
    records m_data;
    records::size_type m_position;
    std::vector<std::string> m_columns;
    
    void check_error(std::string const& msg)
    {
        const char *zErrMsg = sqlite3_errmsg(m_session);
        std::ostringstream ss;
        ss << msg << " sqlite error: " << zErrMsg;
        throw sqlite_driver_error(ss.str());        
    }
    
    bool spatialite()
    {
        int code = sqlite3_enable_load_extension(m_session, 1);
        if (code != SQLITE_OK)
        {
            std::ostringstream oss;
            oss << "Unable to load spatialite extension!";
            throw sqlite_driver_error(oss.str());
        }

        std::ostringstream oss;
        oss << "SELECT load_extension('libspatialite.dylib')";
        execute(oss.str());
        oss.str("");

        oss << "SELECT InitSpatialMetadata()";
        execute(oss.str());
        oss.str("");
        
        return true;

    }
    
};

}
}
} // namespace pdal::driver::soci
