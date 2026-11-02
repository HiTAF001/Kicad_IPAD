/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Gerber reading follows the Ucamco Gerber Layer Format Specification; the older forms still
 * found in the wild (single quadrant arcs, trailing zero omission, G54, inch units, step and
 * repeat) are understood too. Excellon reading covers the drill files CAD tools write.
 */
#include "gerber.h"
#include "i18n.h"
#include "sexpr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <zlib.h>

#include "../third_party/earcut.hpp"

namespace mapbox
{
namespace util
{
template <>
struct nth<0, kp::Pt>
{
    inline static double get( const kp::Pt& t ) { return t.x; }
};

template <>
struct nth<1, kp::Pt>
{
    inline static double get( const kp::Pt& t ) { return t.y; }
};
} // namespace util
} // namespace mapbox

namespace kp
{

namespace
{

constexpr double PI_ = 3.14159265358979323846;
constexpr double ARC_ERR = 0.002; // mm

std::string lower( std::string s )
{
    for( char& c : s )
        c = (char) std::tolower( (unsigned char) c );

    return s;
}

bool endsWith( const std::string& s, const std::string& e )
{
    return s.size() >= e.size() && s.compare( s.size() - e.size(), e.size(), e ) == 0;
}

/// Board frame (Y down) from Gerber frame (Y up).
Pt toBoard( double x, double y )
{
    return Pt( x, -y );
}

void arcPoints( Path& out, const Pt& c, double r, double a0, double a1 )
{
    double sweep = a1 - a0;
    int    n = std::max( 2, (int) std::ceil( std::fabs( sweep ) / ( 2 * std::acos( std::max( -1.0, 1 - ARC_ERR / std::max( r, 1e-6 ) ) ) ) ) );
    n = std::min( n, 720 );

    for( int k = 1; k <= n; ++k )
    {
        double a = a0 + sweep * k / n;
        out.push_back( Pt( c.x + r * std::cos( a ), c.y + r * std::sin( a ) ) );
    }
}

Pt rot( const Pt& p, double deg )
{
    double a = deg * PI_ / 180, c = std::cos( a ), s = std::sin( a );
    return Pt( p.x * c - p.y * s, p.x * s + p.y * c );
}


Path circlePath( const Pt& c, double r )
{
    Path p;
    int  n = std::max( 16, CircleSegments( r, ARC_ERR ) );

    for( int k = 0; k < n; ++k )
        p.push_back( Pt( c.x + r * std::cos( 2 * PI_ * k / n ), c.y + r * std::sin( 2 * PI_ * k / n ) ) );

    return p;
}


// ------------------------------------------------------------------------------------------
// Aperture macro expressions: numbers, $n, + - x / and parentheses
// ------------------------------------------------------------------------------------------

class Expr
{
public:
    Expr( const std::string& s, const std::vector<double>& vars ) :
            m_s( s ),
            m_vars( vars )
    {
    }

    double Eval()
    {
        double v = sum();
        return std::isfinite( v ) ? v : 0.0;
    }

private:
    void ws()
    {
        while( m_i < m_s.size() && std::isspace( (unsigned char) m_s[m_i] ) )
            ++m_i;
    }

    double sum()
    {
        double v = product();

        for( ;; )
        {
            ws();

            if( m_i < m_s.size() && m_s[m_i] == '+' )
            {
                ++m_i;
                v += product();
            }
            else if( m_i < m_s.size() && m_s[m_i] == '-' )
            {
                ++m_i;
                v -= product();
            }
            else
            {
                return v;
            }
        }
    }

    double product()
    {
        double v = unary();

        for( ;; )
        {
            ws();

            if( m_i < m_s.size() && ( m_s[m_i] == 'x' || m_s[m_i] == 'X' ) )
            {
                ++m_i;
                v *= unary();
            }
            else if( m_i < m_s.size() && m_s[m_i] == '/' )
            {
                ++m_i;
                double d = unary();
                v = d != 0 ? v / d : 0;
            }
            else
            {
                return v;
            }
        }
    }

    double unary()
    {
        ws();

        if( m_i < m_s.size() && m_s[m_i] == '-' )
        {
            ++m_i;
            return -unary();
        }

        if( m_i < m_s.size() && m_s[m_i] == '+' )
        {
            ++m_i;
            return unary();
        }

        return atom();
    }

    double atom()
    {
        ws();

        if( m_i >= m_s.size() )
            return 0;

        if( m_s[m_i] == '(' )
        {
            ++m_i;
            double v = sum();
            ws();

            if( m_i < m_s.size() && m_s[m_i] == ')' )
                ++m_i;

            return v;
        }

        if( m_s[m_i] == '$' )
        {
            ++m_i;
            size_t j = m_i;

            while( j < m_s.size() && std::isdigit( (unsigned char) m_s[j] ) )
                ++j;

            int n = std::atoi( m_s.substr( m_i, j - m_i ).c_str() );
            m_i = j;
            return n >= 1 && n <= (int) m_vars.size() ? m_vars[(size_t) n - 1] : 0.0;
        }

        size_t j = m_i;

        while( j < m_s.size() && ( std::isdigit( (unsigned char) m_s[j] ) || m_s[j] == '.' ) )
            ++j;

        double v = std::atof( m_s.substr( m_i, j - m_i ).c_str() );
        m_i = j == m_i ? m_i + 1 : j;
        return v;
    }

    const std::string&         m_s;
    const std::vector<double>& m_vars;
    size_t                     m_i = 0;
};


// ------------------------------------------------------------------------------------------
// Gerber
// ------------------------------------------------------------------------------------------

/// A flashed or drawn element in aperture-local Gerber coordinates (mm).
struct LocalShape
{
    enum Kind
    {
        POLY,
        DISC,
        RING
    } kind = POLY;
    Path   poly;
    Pt     c;
    double r = 0, width = 0;
    bool   clear = false; ///< a hole in the aperture
};

LocalShape polyShape( Path p, bool clear = false )
{
    LocalShape l;
    l.kind = LocalShape::POLY;
    l.poly = std::move( p );
    l.clear = clear;
    return l;
}

LocalShape discShape( const Pt& c, double r, bool clear = false )
{
    LocalShape l;
    l.kind = LocalShape::DISC;
    l.c = c;
    l.r = r;
    l.clear = clear;
    return l;
}

Path circlePath( const Pt& c, double r );
Path sector( double ri, double ro, double a0, double a1 );
void subtract( std::vector<Path>& aSet, const Path& aHole );

/// Apply the holes (exposure off primitives, aperture holes) of an aperture to the shapes
/// before them, so a flash paints exactly the aperture and nothing is erased around it.
std::vector<LocalShape> resolveHoles( const std::vector<LocalShape>& in )
{
    std::vector<LocalShape> out;

    for( const LocalShape& l : in )
    {
        if( !l.clear )
        {
            out.push_back( l );
            continue;
        }

        Path hole = l.kind == LocalShape::POLY ? l.poly : circlePath( l.c, l.r );

        if( l.kind == LocalShape::RING )
            continue; // (no primitive makes these)

        std::vector<LocalShape> next;

        for( const LocalShape& o : out )
        {
            // a round hole in the middle of a disc or ring makes a ring
            if( l.kind == LocalShape::DISC && o.kind != LocalShape::POLY && std::hypot( o.c.x - l.c.x, o.c.y - l.c.y ) < 1e-9 )
            {
                double ro = o.kind == LocalShape::DISC ? o.r : o.r + o.width / 2;
                double ri = o.kind == LocalShape::DISC ? 0.0 : o.r - o.width / 2;
                double hi = std::max( ri, l.r );

                if( hi >= ro )
                    continue; // nothing left

                LocalShape r;
                r.kind = LocalShape::RING;
                r.c = o.c;
                r.r = ( ro + hi ) / 2;
                r.width = ro - hi;
                next.push_back( r );
                continue;
            }

            std::vector<Path> polys;

            if( o.kind == LocalShape::POLY )
            {
                polys.push_back( o.poly );
            }
            else if( o.kind == LocalShape::DISC )
            {
                polys.push_back( circlePath( o.c, o.r ) );
            }
            else
            {
                for( int q = 0; q < 4; ++q )
                {
                    Path sec = sector( o.r - o.width / 2, o.r + o.width / 2, q * PI_ / 2, ( q + 1 ) * PI_ / 2 );

                    for( Pt& pt : sec )
                        pt = Pt( pt.x + o.c.x, pt.y + o.c.y );

                    polys.push_back( sec );
                }
            }

            subtract( polys, hole );

            for( Path& pp : polys )
                next.push_back( polyShape( std::move( pp ) ) );
        }

        out.swap( next );
    }

    return out;
}

/// Convex hull (counter-clockwise), monotone chain.
Path convexHull( Path p )
{
    std::sort( p.begin(), p.end(), []( const Pt& a, const Pt& b ) { return a.x < b.x || ( a.x == b.x && a.y < b.y ); } );

    if( p.size() < 3 )
        return p;

    auto cross = []( const Pt& o, const Pt& a, const Pt& b ) { return ( a.x - o.x ) * ( b.y - o.y ) - ( a.y - o.y ) * ( b.x - o.x ); };
    Path h( 2 * p.size() );
    size_t k = 0;

    for( size_t i = 0; i < p.size(); ++i )
    {
        while( k >= 2 && cross( h[k - 2], h[k - 1], p[i] ) <= 0 )
            --k;

        h[k++] = p[i];
    }

    for( size_t i = p.size() - 1, t = k + 1; i > 0; --i )
    {
        while( k >= t && cross( h[k - 2], h[k - 1], p[i - 1] ) <= 0 )
            --k;

        h[k++] = p[i - 1];
    }

    h.resize( k - 1 );
    return h;
}

double signedArea( const Path& p )
{
    double a = 0;

    for( size_t i = 0, n = p.size(); i < n; ++i )
        a += p[i].x * p[( i + 1 ) % n].y - p[( i + 1 ) % n].x * p[i].y;

    return a / 2;
}

/// Sutherland-Hodgman: the part of p left (aLeft) or right of the line a -> b.
Path clipHalf( const Path& p, const Pt& a, const Pt& b, bool aLeft )
{
    Path out;
    auto side = [&]( const Pt& q )
    {
        double c = ( b.x - a.x ) * ( q.y - a.y ) - ( b.y - a.y ) * ( q.x - a.x );
        return aLeft ? c : -c;
    };

    for( size_t i = 0, n = p.size(); i < n; ++i )
    {
        const Pt& u = p[i];
        const Pt& v = p[( i + 1 ) % n];
        double    su = side( u ), sv = side( v );

        if( su >= 0 )
            out.push_back( u );

        if( ( su >= 0 ) != ( sv >= 0 ) )
        {
            double t = su / ( su - sv );
            out.push_back( Pt( u.x + ( v.x - u.x ) * t, u.y + ( v.y - u.y ) * t ) );
        }
    }

    return out;
}

BBox pathBox( const Path& p )
{
    BBox b;

    for( const Pt& q : p )
        b.add( q );

    return b;
}

/// a minus the convex polygon b, as pieces (which together cover exactly a - b).
void subtractConvex( const Path& a, Path b, std::vector<Path>& out )
{
    BBox ba = pathBox( a ), bb = pathBox( b );

    if( ba.maxX <= bb.minX || bb.maxX <= ba.minX || ba.maxY <= bb.minY || bb.maxY <= ba.minY )
    {
        out.push_back( a );
        return;
    }

    if( signedArea( b ) < 0 )
        std::reverse( b.begin(), b.end() );

    Path rest = a;

    for( size_t i = 0, n = b.size(); i < n && rest.size() >= 3; ++i )
    {
        Path outside = clipHalf( rest, b[i], b[( i + 1 ) % n], false );

        if( outside.size() >= 3 && std::fabs( signedArea( outside ) ) > 1e-12 )
            out.push_back( std::move( outside ) );

        rest = clipHalf( rest, b[i], b[( i + 1 ) % n], true );
    }
}

/// Every polygon of aSet minus aHole (any simple polygon: cut as triangles).
void subtract( std::vector<Path>& aSet, const Path& aHole )
{
    if( aHole.size() < 3 )
        return;

    std::vector<Path> convex;
    Path              hull = convexHull( aHole );

    if( std::fabs( std::fabs( signedArea( hull ) ) - std::fabs( signedArea( aHole ) ) ) < 1e-12 )
    {
        convex.push_back( hull );
    }
    else
    {
        std::vector<Path>     rings { aHole };
        std::vector<uint32_t> idx = mapbox::earcut<uint32_t>( rings );

        for( size_t k = 0; k + 2 < idx.size(); k += 3 )
            convex.push_back( { aHole[idx[k]], aHole[idx[k + 1]], aHole[idx[k + 2]] } );
    }

    for( const Path& c : convex )
    {
        std::vector<Path> next;

        for( const Path& a : aSet )
            subtractConvex( a, c, next );

        aSet.swap( next );
    }
}

/// Ring sector between radii ri and ro from angle a0 to a1 (radians, a1 > a0).
Path sector( double ri, double ro, double a0, double a1 )
{
    Path p;
    int  n = std::max( 2, (int) std::ceil( ( a1 - a0 ) / ( 2 * PI_ ) * CircleSegments( ro, ARC_ERR ) ) );

    for( int k = 0; k <= n; ++k )
    {
        double a = a0 + ( a1 - a0 ) * k / n;
        p.push_back( Pt( ro * std::cos( a ), ro * std::sin( a ) ) );
    }

    if( ri > 0 )
    {
        for( int k = n; k >= 0; --k )
        {
            double a = a0 + ( a1 - a0 ) * k / n;
            p.push_back( Pt( ri * std::cos( a ), ri * std::sin( a ) ) );
        }
    }
    else
    {
        p.push_back( Pt( 0, 0 ) );
    }

    return p;
}

class GerberReader
{
public:
    GerberReader( Board& b, int layer ) :
            m_b( b ),
            m_layer( layer )
    {
    }

    void Parse( const std::string& d )
    {
        size_t i = 0, n = d.size();
        m_firstShape = m_b.shapes.size();

        while( i < n )
        {
            char c = d[i];

            if( c == '%' )
            {
                size_t e = d.find( '%', i + 1 );

                if( e == std::string::npos )
                    break;

                extended( d.substr( i + 1, e - i - 1 ) );
                i = e + 1;
            }
            else if( std::isspace( (unsigned char) c ) )
            {
                ++i;
            }
            else
            {
                size_t e = d.find( '*', i );

                if( e == std::string::npos )
                    break;

                if( !word( d.substr( i, e - i ) ) )
                    break; // M02

                i = e + 1;
            }
        }

        flushContour();
        finishRepeat();
        finishNegative();
    }

    bool        usedClear = false;
    int         objects = 0;
    std::string fileFunction;

private:
    struct Aperture
    {
        char                type = 'C'; // C R O P, or M for a macro
        std::vector<double> p;
        std::string         macro;
    };

    double scale() const { return m_mm ? 1.0 : 25.4; }

    // ---- extended (%...%) commands
    void extended( std::string block )
    {
        block.erase( std::remove_if( block.begin(), block.end(), []( char ch ) { return ch == '\n' || ch == '\r'; } ),
                     block.end() );

        if( block.size() < 2 )
            return;

        std::string code = block.substr( 0, 2 );

        if( code == "AM" )
        {
            // AMname*prim*prim*...*
            size_t      star = block.find( '*' );
            std::string name = block.substr( 2, star == std::string::npos ? std::string::npos : star - 2 );
            std::vector<std::string> body;

            for( size_t s = star; s != std::string::npos && s + 1 < block.size(); )
            {
                size_t e = block.find( '*', s + 1 );
                std::string prim = block.substr( s + 1, e == std::string::npos ? std::string::npos : e - s - 1 );

                if( !prim.empty() )
                    body.push_back( prim );

                s = e;
            }

            m_macros[name] = body;
            return;
        }

        // other blocks may hold several commands, each ending with '*'
        size_t s = 0;

        while( s < block.size() )
        {
            size_t      e = block.find( '*', s );
            std::string cmd = block.substr( s, e == std::string::npos ? std::string::npos : e - s );

            if( cmd.size() >= 2 )
                command( cmd );

            if( e == std::string::npos )
                break;

            s = e + 1;
        }
    }

    void command( const std::string& cmd )
    {
        std::string code = cmd.substr( 0, 2 );

        if( code == "FS" )
        {
            // FS[L|T][A|I]Xnn Ynn
            m_trailing = cmd.find( 'T' ) != std::string::npos && cmd.find( 'T' ) < cmd.find( 'X' );
            size_t x = cmd.find( 'X' ), y = cmd.find( 'Y' );

            if( x != std::string::npos && x + 2 < cmd.size() )
            {
                m_xi = cmd[x + 1] - '0';
                m_xd = cmd[x + 2] - '0';
            }

            if( y != std::string::npos && y + 2 < cmd.size() )
            {
                m_yi = cmd[y + 1] - '0';
                m_yd = cmd[y + 2] - '0';
            }
        }
        else if( code == "MO" )
        {
            m_mm = cmd.compare( 2, 2, "MM" ) == 0;
        }
        else if( code == "AD" )
        {
            aperture( cmd.substr( 2 ) );
        }
        else if( code == "LP" )
        {
            m_dark = cmd.size() < 3 || cmd[2] != 'C';
        }
        else if( code == "IP" )
        {
            m_negative = cmd.find( "NEG" ) != std::string::npos;
        }
        else if( code == "SR" )
        {
            finishRepeat();
            m_srX = m_srY = 1;
            m_srI = m_srJ = 0;
            auto val = [&]( char k ) -> double
            {
                size_t p = cmd.find( k );
                return p == std::string::npos ? 0.0 : std::atof( cmd.c_str() + p + 1 );
            };

            if( cmd.size() > 2 )
            {
                m_srX = std::max( 1, (int) val( 'X' ) );
                m_srY = std::max( 1, (int) val( 'Y' ) );
                m_srI = val( 'I' ) * scale();
                m_srJ = val( 'J' ) * scale();
            }

            m_srStart = m_b.shapes.size();
        }
        else if( code == "TF" )
        {
            if( cmd.compare( 0, 15, "TF.FileFunction" ) == 0 && cmd.size() > 16 )
                fileFunction = cmd.substr( 16 );
        }
        // IP, OF, SF, IN, LN, TA, TO, TD, AS, MI, IR, LM, LR, LS: nothing to draw
    }

    void aperture( const std::string& def )
    {
        // Dnn<type>[,p1Xp2...]
        if( def.empty() || def[0] != 'D' )
            return;

        size_t i = 1;

        while( i < def.size() && std::isdigit( (unsigned char) def[i] ) )
            ++i;

        int         d = std::atoi( def.substr( 1, i - 1 ).c_str() );
        size_t      comma = def.find( ',', i );
        std::string type = def.substr( i, comma == std::string::npos ? std::string::npos : comma - i );
        Aperture    a;

        if( comma != std::string::npos )
        {
            std::string params = def.substr( comma + 1 );
            size_t      s = 0;

            while( s <= params.size() )
            {
                size_t x = params.find_first_of( "Xx", s );
                a.p.push_back( std::atof( params.substr( s, x == std::string::npos ? std::string::npos : x - s ).c_str() ) );

                if( x == std::string::npos )
                    break;

                s = x + 1;
            }
        }

        if( type == "C" || type == "R" || type == "O" || type == "P" )
        {
            a.type = type[0];

            // sizes scale with the units, rotation and vertex count do not
            for( size_t k = 0; k < a.p.size(); ++k )
            {
                bool notSize = a.type == 'P' && ( k == 1 || k == 2 );

                if( !notSize )
                    a.p[k] *= scale();
            }
        }
        else
        {
            a.type = 'M';
            a.macro = type;
        }

        m_apertures[d] = a;
        m_flashCache.erase( d );
    }

    // ---- word commands
    double coord( const std::string& s, bool isX ) const
    {
        if( s.find( '.' ) != std::string::npos )
            return std::atof( s.c_str() ) * scale();

        bool        neg = !s.empty() && s[0] == '-';
        std::string digits = ( !s.empty() && ( s[0] == '-' || s[0] == '+' ) ) ? s.substr( 1 ) : s;
        int         ints = isX ? m_xi : m_yi, decs = isX ? m_xd : m_yd;

        if( m_trailing )
        {
            while( (int) digits.size() < ints + decs )
                digits += '0';
        }

        double v = std::atof( digits.c_str() ) / std::pow( 10.0, decs );
        return ( neg ? -v : v ) * scale();
    }

    bool word( const std::string& w )
    {
        if( w.empty() )
            return true;

        if( w.compare( 0, 3, "G04" ) == 0 || w.compare( 0, 2, "G4" ) == 0 )
            return true; // comment

        std::string x, y, i, j;
        int         dcode = -1;
        size_t      p = 0;

        while( p < w.size() )
        {
            char   k = (char) std::toupper( (unsigned char) w[p++] );
            size_t s = p;

            while( p < w.size() && ( std::isdigit( (unsigned char) w[p] ) || w[p] == '-' || w[p] == '+' || w[p] == '.' ) )
                ++p;

            std::string v = w.substr( s, p - s );

            switch( k )
            {
            case 'G':
            {
                int g = std::atoi( v.c_str() );

                if( g == 1 || g == 2 || g == 3 )
                    m_interp = g;
                else if( g == 36 )
                {
                    m_region = true;
                    m_contour.clear();
                }
                else if( g == 37 )
                {
                    flushContour();
                    m_region = false;
                }
                else if( g == 74 )
                    m_multi = false;
                else if( g == 75 )
                    m_multi = true;
                else if( g == 70 )
                    m_mm = false;
                else if( g == 71 )
                    m_mm = true;

                break;
            }
            case 'D': dcode = std::atoi( v.c_str() ); break;
            case 'M':
                if( std::atoi( v.c_str() ) == 2 || std::atoi( v.c_str() ) == 0 )
                    return false;

                break;
            case 'X': x = v; break;
            case 'Y': y = v; break;
            case 'I': i = v; break;
            case 'J': j = v; break;
            default: break;
            }
        }

        if( dcode >= 10 )
        {
            m_aperture = dcode;
            dcode = -1;
        }

        bool hasCoord = !x.empty() || !y.empty() || !i.empty() || !j.empty();

        if( dcode < 0 && hasCoord )
            dcode = m_lastOp; // deprecated modal operation

        if( dcode < 1 || dcode > 3 )
            return true;

        m_lastOp = dcode;
        Pt next( x.empty() ? m_cur.x : coord( x, true ), y.empty() ? m_cur.y : coord( y, false ) );
        Pt off( i.empty() ? 0.0 : coord( i, true ), j.empty() ? 0.0 : coord( j, false ) );

        if( dcode == 1 )
            interpolate( next, off );
        else if( dcode == 2 )
        {
            if( m_region )
            {
                flushContour();
                m_contour = { next };
            }
        }
        else
        {
            flash( next );
        }

        m_cur = next;
        return true;
    }

    /// Points of the segment or arc from the current point to aEnd (Gerber frame), excluding
    /// the start point.
    void path( const Pt& end, const Pt& off, Path& out )
    {
        if( m_interp == 1 )
        {
            out.push_back( end );
            return;
        }

        bool cw = m_interp == 2;
        Pt   s = m_cur;
        Pt   c;

        if( m_multi )
        {
            c = Pt( s.x + off.x, s.y + off.y );
        }
        else
        {
            // single quadrant: the centre is one of four, the one giving an arc of at most 90°
            double best = 1e300;

            for( int sx : { 1, -1 } )
            {
                for( int sy : { 1, -1 } )
                {
                    Pt     cc( s.x + sx * std::fabs( off.x ), s.y + sy * std::fabs( off.y ) );
                    double r0 = std::hypot( s.x - cc.x, s.y - cc.y ), r1 = std::hypot( end.x - cc.x, end.y - cc.y );
                    double a0 = std::atan2( s.y - cc.y, s.x - cc.x ), a1 = std::atan2( end.y - cc.y, end.x - cc.x );
                    double sweep = cw ? a0 - a1 : a1 - a0;

                    while( sweep < 0 )
                        sweep += 2 * PI_;

                    if( sweep > PI_ / 2 + 1e-6 )
                        continue;

                    double err = std::fabs( r0 - r1 );

                    if( err < best )
                    {
                        best = err;
                        c = cc;
                    }
                }
            }

            if( best > 1e299 )
            {
                out.push_back( end );
                return;
            }
        }

        double r = std::hypot( s.x - c.x, s.y - c.y );
        double a0 = std::atan2( s.y - c.y, s.x - c.x ), a1 = std::atan2( end.y - c.y, end.x - c.x );
        bool   full = m_multi && std::hypot( end.x - s.x, end.y - s.y ) < 1e-9;

        if( cw )
        {
            if( full )
                a1 = a0 - 2 * PI_;
            else
                while( a1 >= a0 )
                    a1 -= 2 * PI_;
        }
        else
        {
            if( full )
                a1 = a0 + 2 * PI_;
            else
                while( a1 <= a0 )
                    a1 += 2 * PI_;
        }

        arcPoints( out, c, r, a0, a1 );
        out.back() = end;
    }

    void interpolate( const Pt& end, const Pt& off )
    {
        if( m_region )
        {
            if( m_contour.empty() )
                m_contour.push_back( m_cur );

            path( end, off, m_contour );
            return;
        }

        auto it = m_apertures.find( m_aperture );

        if( it == m_apertures.end() )
            return; // no aperture selected: nothing is drawn

        const Aperture& ap = it->second;
        double          width = 0;

        if( !ap.p.empty() )
            width = ap.type == 'R' || ap.type == 'O' ? std::min( ap.p[0], ap.p.size() > 1 ? ap.p[1] : ap.p[0] ) : ap.p[0];

        if( m_interp == 1 && ( ap.type == 'R' || ap.type == 'O' || ap.type == 'P' ) )
        {
            // a non-round aperture sweeps the hull of its shape at both ends
            std::vector<LocalShape> parts;
            apertureShapes( ap, parts );
            Path pts;

            for( const LocalShape& l : parts )
            {
                if( l.kind != LocalShape::POLY || l.clear )
                    continue;

                for( const Pt& q : l.poly )
                {
                    pts.push_back( Pt( m_cur.x + q.x, m_cur.y + q.y ) );
                    pts.push_back( Pt( end.x + q.x, end.y + q.y ) );
                }
            }

            Path  hull = convexHull( pts );
            Shape s;
            s.type = ShapeType::POLY;
            s.filled = true;

            for( const Pt& q : hull )
                s.poly.push_back( toBoard( q.x, q.y ) );

            if( s.poly.size() >= 3 )
                add( s );

            return;
        }

        if( ap.type == 'M' )
        {
            // drawing with a macro aperture is not allowed; use the size of its first primitive
            std::vector<LocalShape> parts;
            apertureShapes( ap, parts );
            BBox box;

            for( const LocalShape& l : parts )
            {
                if( l.kind == LocalShape::POLY )
                {
                    for( const Pt& q : l.poly )
                        box.add( q );
                }
                else
                {
                    box.add( l.c, l.r );
                }
            }

            width = box.valid() ? std::min( box.maxX - box.minX, box.maxY - box.minY ) : 0;
        }

        if( m_interp == 1 )
        {
            Shape s;
            s.type = ShapeType::SEGMENT;
            s.start = toBoard( m_cur.x, m_cur.y );
            s.end = toBoard( end.x, end.y );
            s.width = width;
            add( s );
        }
        else
        {
            Path pts { m_cur };
            path( end, off, pts );
            Shape s;
            s.width = width;

            if( m_multi && std::hypot( end.x - m_cur.x, end.y - m_cur.y ) < 1e-9 )
            {
                // full circle
                s.type = ShapeType::CIRCLE;
                s.start = toBoard( m_cur.x + off.x, m_cur.y + off.y );
                s.end = toBoard( m_cur.x, m_cur.y );
            }
            else if( pts.size() >= 3 )
            {
                s.type = ShapeType::ARC;
                s.start = toBoard( m_cur.x, m_cur.y );
                s.mid = toBoard( pts[pts.size() / 2].x, pts[pts.size() / 2].y );
                s.end = toBoard( end.x, end.y );
            }
            else
            {
                s.type = ShapeType::SEGMENT;
                s.start = toBoard( m_cur.x, m_cur.y );
                s.end = toBoard( end.x, end.y );
            }

            add( s );
        }
    }

    void flushContour()
    {
        if( m_contour.size() >= 3 )
        {
            Shape s;
            s.type = ShapeType::POLY;
            s.filled = true;

            for( const Pt& q : m_contour )
                s.poly.push_back( toBoard( q.x, q.y ) );

            add( s );
        }

        m_contour.clear();
    }

    /// The shapes a flash of aperture aCode paints (holes already cut out), cached.
    const std::vector<LocalShape>* flashShapes( int aCode )
    {
        auto cached = m_flashCache.find( aCode );

        if( cached != m_flashCache.end() )
            return &cached->second;

        auto it = m_apertures.find( aCode );

        if( it == m_apertures.end() )
            return nullptr;

        std::vector<LocalShape> parts;
        apertureShapes( it->second, parts );
        return &( m_flashCache[aCode] = resolveHoles( parts ) );
    }

    void flash( const Pt& at )
    {
        const std::vector<LocalShape>* parts = flashShapes( m_aperture );

        if( !parts )
            return;

        for( const LocalShape& l : *parts )
        {
            Shape s;

            if( l.kind == LocalShape::POLY )
            {
                if( l.poly.size() < 3 )
                    continue;

                s.type = ShapeType::POLY;
                s.filled = true;

                for( const Pt& q : l.poly )
                    s.poly.push_back( toBoard( at.x + q.x, at.y + q.y ) );
            }
            else
            {
                if( l.r <= 0 && l.width <= 0 )
                    continue;

                s.type = ShapeType::CIRCLE;
                s.start = toBoard( at.x + l.c.x, at.y + l.c.y );
                s.end = s.start + Pt( l.r, 0 );
                s.filled = l.kind == LocalShape::DISC;
                s.width = l.width;
            }

            add( s );
        }
    }

    void apertureShapes( const Aperture& a, std::vector<LocalShape>& out )
    {
        auto P = [&]( size_t k, double def = 0.0 ) { return k < a.p.size() ? a.p[k] : def; };

        // standard apertures may have a round hole (one parameter) or, in old files, a
        // rectangular one (two parameters)
        auto hole = [&]( size_t k )
        {
            if( P( k ) <= 0 )
                return;

            if( a.p.size() > k + 1 && P( k + 1 ) > 0 )
            {
                double hx = P( k ) / 2, hy = P( k + 1 ) / 2;
                out.push_back( polyShape( { Pt( -hx, -hy ), Pt( hx, -hy ), Pt( hx, hy ), Pt( -hx, hy ) }, true ) );
            }
            else
            {
                out.push_back( discShape( Pt(), P( k ) / 2, true ) );
            }
        };

        switch( a.type )
        {
        case 'C':
            out.push_back( discShape( Pt(), P( 0 ) / 2 ) );
            hole( 1 );
            break;

        case 'R':
        {
            double hx = P( 0 ) / 2, hy = P( 1, P( 0 ) ) / 2;
            out.push_back( polyShape( { Pt( -hx, -hy ), Pt( hx, -hy ), Pt( hx, hy ), Pt( -hx, hy ) } ) );
            hole( 2 );
            break;
        }
        case 'O':
        {
            double w = P( 0 ), h = P( 1, P( 0 ) );
            double r = std::min( w, h ) / 2, half = ( std::max( w, h ) - std::min( w, h ) ) / 2;
            Path   p;
            int    n = std::max( 8, CircleSegments( r, ARC_ERR ) / 2 );

            // two half circles joined by straight sides
            for( int side = 0; side < 2; ++side )
            {
                for( int k = 0; k <= n; ++k )
                {
                    double ang = ( w >= h ? -PI_ / 2 : 0 ) + PI_ * side + PI_ * k / n;
                    Pt     c = w >= h ? Pt( side == 0 ? half : -half, 0 ) : Pt( 0, side == 0 ? half : -half );
                    p.push_back( Pt( c.x + r * std::cos( ang ), c.y + r * std::sin( ang ) ) );
                }
            }

            out.push_back( polyShape( p ) );
            hole( 2 );
            break;
        }
        case 'P':
        {
            double r = P( 0 ) / 2;
            int    n = std::max( 3, (int) P( 1, 3 ) );
            double rotation = P( 2 );
            Path   p;

            for( int k = 0; k < n; ++k )
                p.push_back( rot( Pt( r, 0 ), rotation + 360.0 * k / n ) );

            out.push_back( polyShape( p ) );
            hole( 3 );
            break;
        }
        default: macroShapes( a, out ); break;
        }
    }

    void macroShapes( const Aperture& a, std::vector<LocalShape>& out )
    {
        auto it = m_macros.find( a.macro );

        if( it == m_macros.end() )
            return;

        std::vector<double> vars = a.p;
        double              k = scale();

        for( const std::string& prim : it->second )
        {
            if( prim.empty() || prim[0] == '0' )
                continue; // comment

            if( prim[0] == '$' )
            {
                // $n=expression
                size_t eq = prim.find( '=' );

                if( eq != std::string::npos )
                {
                    int n = std::atoi( prim.c_str() + 1 );

                    if( n >= 1 && n <= 1000 )
                    {
                        if( (int) vars.size() < n )
                            vars.resize( (size_t) n, 0.0 );

                        std::string e = prim.substr( eq + 1 );
                        vars[(size_t) n - 1] = Expr( e, vars ).Eval();
                    }
                }

                continue;
            }

            std::vector<double> v;
            size_t              s = 0;

            while( s <= prim.size() )
            {
                size_t      c = prim.find( ',', s );
                std::string e = prim.substr( s, c == std::string::npos ? std::string::npos : c - s );
                v.push_back( Expr( e, vars ).Eval() );

                if( c == std::string::npos )
                    break;

                s = c + 1;
            }

            if( v.empty() )
                continue;

            int  code = (int) v[0];
            auto V = [&]( size_t i ) { return i < v.size() ? v[i] : 0.0; };
            bool off = code != 6 && code != 7 && V( 1 ) == 0; // exposure off: a hole in the aperture

            switch( code )
            {
            case 1:
                out.push_back( discShape( rot( Pt( V( 3 ) * k, V( 4 ) * k ), V( 5 ) ), V( 2 ) * k / 2, off ) );
                break;

            case 2:
            case 20:
            {
                Pt     a1( V( 3 ) * k, V( 4 ) * k ), a2( V( 5 ) * k, V( 6 ) * k );
                double w = V( 2 ) * k / 2;
                Pt     d = a2 - a1;
                double len = std::hypot( d.x, d.y );

                if( len < 1e-9 )
                    break;

                Pt     n( -d.y / len * w, d.x / len * w );
                double r = V( 7 );
                out.push_back( polyShape( { rot( a1 + n, r ), rot( a2 + n, r ), rot( a2 - n, r ), rot( a1 - n, r ) }, off ) );
                break;
            }
            case 21:
            case 22:
            {
                double w = V( 2 ) * k, h = V( 3 ) * k, x = V( 4 ) * k, y = V( 5 ) * k, r = V( 6 );
                double x0 = code == 21 ? x - w / 2 : x, y0 = code == 21 ? y - h / 2 : y;
                out.push_back( polyShape( { rot( Pt( x0, y0 ), r ), rot( Pt( x0 + w, y0 ), r ), rot( Pt( x0 + w, y0 + h ), r ),
                                            rot( Pt( x0, y0 + h ), r ) },
                                          off ) );
                break;
            }
            case 4:
            {
                int  n = (int) V( 2 );
                Path p;

                for( int i = 0; i <= n && 3 + 2 * i + 1 < (int) v.size(); ++i )
                    p.push_back( Pt( V( (size_t) ( 3 + 2 * i ) ) * k, V( (size_t) ( 4 + 2 * i ) ) * k ) );

                double r = V( (size_t) ( 3 + 2 * ( n + 1 ) ) );

                for( Pt& q : p )
                    q = rot( q, r );

                if( p.size() >= 3 )
                    out.push_back( polyShape( p, off ) );

                break;
            }
            case 5:
            {
                int    n = std::max( 3, (int) V( 2 ) );
                Pt     c( V( 3 ) * k, V( 4 ) * k );
                double r = V( 5 ) * k / 2, rr = V( 6 );
                Path   p;

                for( int i = 0; i < n; ++i )
                    p.push_back( rot( c + rot( Pt( r, 0 ), 360.0 * i / n ), rr ) );

                out.push_back( polyShape( p, off ) );
                break;
            }
            case 6:
            {
                // moiré: centre, outer diameter, ring thickness, gap, ring count, cross-hair
                // thickness and length, rotation
                Pt     c( V( 1 ) * k, V( 2 ) * k );
                double dia = V( 3 ) * k, t = V( 4 ) * k, gap = V( 5 ) * k, ct = V( 7 ) * k, cl = V( 8 ) * k, rr = V( 9 );
                int    rings = std::max( 0, std::min( 100, (int) V( 6 ) ) );

                for( int i = 0; i < rings && dia > 0; ++i )
                {
                    LocalShape l;
                    l.kind = LocalShape::RING;
                    l.c = rot( c, rr );
                    l.width = std::min( t, dia / 2 );
                    l.r = dia / 2 - l.width / 2;
                    out.push_back( l );
                    dia -= 2 * ( t + gap );
                }

                for( int axis = 0; axis < 2; ++axis )
                {
                    double hx = axis == 0 ? cl / 2 : ct / 2, hy = axis == 0 ? ct / 2 : cl / 2;
                    Path   p { c + Pt( -hx, -hy ), c + Pt( hx, -hy ), c + Pt( hx, hy ), c + Pt( -hx, hy ) };

                    for( Pt& q : p )
                        q = rot( q, rr );

                    out.push_back( polyShape( p ) );
                }

                break;
            }
            case 7:
            {
                // thermal: centre, outer and inner diameter, gap, rotation
                Pt     c( V( 1 ) * k, V( 2 ) * k );
                double ro = V( 3 ) * k / 2, ri = V( 4 ) * k / 2, g = V( 5 ) * k / 2, rr = V( 6 );

                if( ro <= g )
                    break;

                for( int q = 0; q < 4; ++q )
                {
                    // one quarter between the gaps, built in the first quadrant
                    double ao = std::asin( std::min( 1.0, g / ro ) );
                    Path   p;

                    if( ri > g * std::sqrt( 2.0 ) )
                    {
                        double ai = std::asin( g / ri );
                        Path   outer = sector( 0, ro, ao, PI_ / 2 - ao );
                        outer.pop_back(); // centre point
                        p = outer;
                        Path inner = sector( 0, ri, ai, PI_ / 2 - ai );
                        inner.pop_back();

                        for( size_t j = inner.size(); j-- > 0; )
                            p.push_back( inner[j] );
                    }
                    else
                    {
                        p = sector( 0, ro, ao, PI_ / 2 - ao );
                        p.back() = Pt( g, g );
                    }

                    for( Pt& pt : p )
                        pt = rot( c + rot( pt, 90.0 * q ), rr );

                    out.push_back( polyShape( p ) );
                }

                break;
            }
            default: break;
            }
        }
    }

    void add( const Shape& s0 )
    {
        Shape s = s0;
        s.layer = m_layer;
        s.clear = !m_dark;
        usedClear |= s.clear;
        m_b.shapes.push_back( s );
        ++objects;
    }

    /// Negative image: everything drawn so far clears a dark area around the image.
    void finishNegative()
    {
        if( !m_negative || m_b.shapes.size() <= m_firstShape )
            return;

        BBox box;

        for( size_t i = m_firstShape; i < m_b.shapes.size(); ++i )
        {
            Shape& s = m_b.shapes[i];
            s.clear = !s.clear;

            for( const Pt& q : s.poly )
                box.add( q, s.width / 2 );

            if( s.type != ShapeType::POLY )
            {
                double r = s.type == ShapeType::CIRCLE ? std::hypot( s.end.x - s.start.x, s.end.y - s.start.y ) : 0;
                box.add( s.start, r + s.width / 2 );
                box.add( s.end, s.width / 2 );
            }
        }

        double m = std::max( 1.0, 0.02 * std::max( box.maxX - box.minX, box.maxY - box.minY ) );
        Shape  bg;
        bg.type = ShapeType::POLY;
        bg.filled = true;
        bg.layer = m_layer;
        bg.poly = { Pt( box.minX - m, box.minY - m ), Pt( box.maxX + m, box.minY - m ), Pt( box.maxX + m, box.maxY + m ),
                    Pt( box.minX - m, box.maxY + m ) };
        m_b.shapes.insert( m_b.shapes.begin() + (std::ptrdiff_t) m_firstShape, bg );
    }

    void finishRepeat()
    {
        if( m_srX <= 1 && m_srY <= 1 )
            return;

        size_t end = m_b.shapes.size();

        for( int ix = 0; ix < m_srX; ++ix )
        {
            for( int iy = 0; iy < m_srY; ++iy )
            {
                if( ix == 0 && iy == 0 )
                    continue;

                Pt d( ix * m_srI, -iy * m_srJ );

                for( size_t k = m_srStart; k < end; ++k )
                {
                    Shape s = m_b.shapes[k];
                    s.start = s.start + d;
                    s.mid = s.mid + d;
                    s.end = s.end + d;

                    for( Pt& q : s.poly )
                        q = q + d;

                    m_b.shapes.push_back( s );
                }
            }
        }

        m_srX = m_srY = 1;
    }

    Board& m_b;
    int    m_layer;
    int    m_xi = 3, m_xd = 6, m_yi = 3, m_yd = 6;
    bool   m_trailing = false;
    bool   m_mm = false; // RS-274X files without a unit statement are in inches
    bool   m_dark = true;
    bool   m_negative = false;
    size_t m_firstShape = 0;
    bool   m_region = false;
    bool   m_multi = false;
    int    m_interp = 1;
    int    m_aperture = -1;
    int    m_lastOp = 0;
    Pt     m_cur;
    Path   m_contour;
    int    m_srX = 1, m_srY = 1;
    double m_srI = 0, m_srJ = 0;
    size_t m_srStart = 0;
    std::map<int, Aperture>                        m_apertures;
    std::map<int, std::vector<LocalShape>>         m_flashCache;
    std::map<std::string, std::vector<std::string>> m_macros;
};


// ------------------------------------------------------------------------------------------
// Excellon
// ------------------------------------------------------------------------------------------

struct DrillHit
{
    Pt     a, b;
    double dia;
};

class ExcellonReader
{
public:
    void Parse( const std::string& d )
    {
        // KiCad marks decimal files in a comment; plain integers there are whole units
        m_decimal = d.find( "/ decimal}" ) != std::string::npos;
        detectZeros( d );

        size_t s = 0;
        bool   header = false;

        while( s < d.size() )
        {
            size_t      e = d.find_first_of( "\r\n", s );
            std::string line = d.substr( s, e == std::string::npos ? std::string::npos : e - s );
            s = e == std::string::npos ? d.size() : e + 1;

            size_t semi = line.find( ';' );

            if( semi != std::string::npos )
            {
                comment( line.substr( semi + 1 ) );
                line.resize( semi );
            }

            line.erase( std::remove_if( line.begin(), line.end(), []( char c ) { return std::isspace( (unsigned char) c ); } ),
                        line.end() );

            if( line.empty() )
                continue;

            if( line == "M48" )
            {
                header = true;
                continue;
            }

            if( line == "%" || line == "M95" )
            {
                header = false;
                continue;
            }

            if( line == "M30" || line == "M00" )
                break;

            if( line.compare( 0, 6, "METRIC" ) == 0 || line.compare( 0, 4, "INCH" ) == 0 )
            {
                units( line[0] == 'M' );

                if( line.find( "LZ" ) != std::string::npos )
                    m_lz = 1;
                else if( line.find( "TZ" ) != std::string::npos )
                    m_lz = 0;

                size_t dot = line.find( '.' );

                if( dot != std::string::npos )
                {
                    // e.g. METRIC,000.000: digits before and after the point
                    size_t b = line.find_last_of( ',', dot );
                    m_int = (int) ( dot - ( b == std::string::npos ? 0 : b + 1 ) );
                    m_dec = (int) ( line.size() - dot - 1 );
                    m_formatGiven = true;
                }

                continue;
            }

            if( line == "M71" || line == "M72" )
            {
                units( line == "M71" );
                continue;
            }

            if( line == "G90" || line == "G91" )
            {
                m_incremental = line == "G91";
                continue;
            }

            if( line.compare( 0, 3, "ICI" ) == 0 )
            {
                m_incremental = line.find( "ON" ) != std::string::npos;
                continue;
            }

            if( line[0] == 'T' )
            {
                size_t i = 1;

                while( i < line.size() && std::isdigit( (unsigned char) line[i] ) )
                    ++i;

                int    t = std::atoi( line.substr( 1, i - 1 ).c_str() );
                size_t c = line.find( 'C', i );

                // sizes are kept as written: some files give the units only after the tools
                if( c != std::string::npos )
                    m_tools[t] = std::atof( line.c_str() + c + 1 );

                if( !header )
                    m_tool = t;

                continue;
            }

            if( header )
                continue;

            // G93: zero set (an offset for everything after it)
            if( line.compare( 0, 3, "G93" ) == 0 )
            {
                Pt saved = m_cur;
                m_cur = Pt();
                bool inc = m_incremental;
                m_incremental = false;
                m_origin = rawPoint( line.substr( 3 ) );
                m_incremental = inc;
                m_cur = saved;
                continue;
            }

            // route mode: G00 move, M15 tool down, G01/G02/G03 cut, M16 tool up, G05 drilling
            if( line.compare( 0, 3, "G00" ) == 0 )
            {
                m_cur = rawPoint( line.substr( 3 ) );
                m_routing = true;
                continue;
            }

            if( line == "M15" )
            {
                m_down = true;
                continue;
            }

            if( line == "M16" || line == "M17" )
            {
                m_down = false;
                continue;
            }

            if( line == "G05" || line == "G81" )
            {
                m_routing = m_down = false;
                continue;
            }

            if( line.compare( 0, 3, "G02" ) == 0 || line.compare( 0, 3, "G03" ) == 0 )
            {
                m_arc = line[2] == '2' ? 2 : 3;
                line = line.substr( 3 );
            }
            else if( line.compare( 0, 3, "G01" ) == 0 )
            {
                m_arc = 0;
                line = line.substr( 3 );
            }

            if( m_routing && !line.empty() && ( line[0] == 'X' || line[0] == 'Y' ) )
            {
                Pt b = rawPoint( line );

                if( m_down )
                {
                    if( m_arc )
                        arcCut( line, b );
                    else
                        hit( m_cur, b );
                }

                m_cur = b;
                continue;
            }

            if( !line.empty() && line[0] == 'R' )
            {
                // R<count>X<dx>Y<dy>: repeat the last hole
                size_t i = 1;

                while( i < line.size() && std::isdigit( (unsigned char) line[i] ) )
                    ++i;

                int count = std::min( 10000, std::atoi( line.substr( 1, i - 1 ).c_str() ) );
                Pt  step = delta( line.substr( i ) );

                for( int k = 0; k < count; ++k )
                {
                    m_cur = Pt( m_cur.x + step.x, m_cur.y + step.y );
                    hit( m_cur, m_cur );
                }

                continue;
            }

            if( !line.empty() && ( line[0] == 'X' || line[0] == 'Y' || line.compare( 0, 3, "G85" ) == 0 ) )
            {
                size_t g85 = line.find( "G85" );
                Pt     a = g85 == 0 ? m_cur : rawPoint( g85 == std::string::npos ? line : line.substr( 0, g85 ) );
                m_cur = a;

                if( g85 != std::string::npos )
                {
                    Pt b = rawPoint( line.substr( g85 + 3 ) );
                    hit( a, b );
                    m_cur = b;
                }
                else
                {
                    hit( a, a );
                }
            }
        }
    }

    std::vector<DrillHit> hits;

private:
    void units( bool aMetric )
    {
        m_mm = aMetric;

        if( !m_formatGiven )
        {
            m_int = m_mm ? 3 : 2;
            m_dec = m_mm ? 3 : 4;
        }
    }

    /// Without LZ / TZ in the header, look at the coordinates: a written leading zero means
    /// the trailing ones are left out.
    void detectZeros( const std::string& d )
    {
        int leading = 0, trailing = 0;

        for( size_t i = 0; i + 1 < d.size(); ++i )
        {
            if( ( d[i] != 'X' && d[i] != 'Y' ) || ( i > 0 && std::isalpha( (unsigned char) d[i - 1] ) && d[i - 1] != 'X' && d[i - 1] != 'Y'
                                                     && d[i - 1] != 'G' && !std::isdigit( (unsigned char) d[i - 1] ) ) )
                continue;

            size_t j = i + 1;

            if( j < d.size() && ( d[j] == '-' || d[j] == '+' ) )
                ++j;

            size_t k = j;

            while( k < d.size() && std::isdigit( (unsigned char) d[k] ) )
                ++k;

            if( k < d.size() && d[k] == '.' )
                continue;

            if( k - j >= 2 && d[j] == '0' )
                ++leading;
            else if( k - j >= 2 && d[k - 1] == '0' )
                ++trailing;
        }

        m_lzGuess = leading > trailing;
    }

    /// Digit counts written as comments: Altium ";FILE_FORMAT=2:5", KiCad
    /// "; FORMAT={3:3/ absolute / metric / ...}". The last valid statement wins.
    void comment( const std::string& c )
    {
        size_t at = c.find( "FILE_FORMAT" );
        size_t from = std::string::npos;

        if( at != std::string::npos )
            from = c.find( '=', at );
        else if( ( at = c.find( "FORMAT={" ) ) != std::string::npos )
            from = at + 7;

        if( from == std::string::npos )
            return;

        int    a = -1, b = -1;
        size_t i = from + 1;

        while( i < c.size() && c[i] == ' ' )
            ++i;

        if( i < c.size() && std::isdigit( (unsigned char) c[i] ) )
            a = c[i++] - '0';

        while( i < c.size() && c[i] == ' ' )
            ++i;

        if( i < c.size() && c[i] == ':' )
            ++i;
        else
            return;

        while( i < c.size() && c[i] == ' ' )
            ++i;

        if( i < c.size() && std::isdigit( (unsigned char) c[i] ) )
            b = c[i] - '0';

        if( a >= 1 && b >= 1 )
        {
            m_int = a;
            m_dec = b;
            m_formatGiven = true;
        }
    }

    double unit() const { return m_mm ? 1.0 : 25.4; }

    double number( const std::string& s ) const
    {
        if( m_decimal || s.find( '.' ) != std::string::npos )
            return std::atof( s.c_str() ) * unit();

        bool        neg = !s.empty() && s[0] == '-';
        std::string digits = ( !s.empty() && ( s[0] == '-' || s[0] == '+' ) ) ? s.substr( 1 ) : s;
        bool        lz = m_lz >= 0 ? m_lz == 1 : m_lzGuess;

        if( lz )
        {
            while( (int) digits.size() < m_int + m_dec )
                digits += '0';
        }

        double v = std::atof( digits.c_str() ) / std::pow( 10.0, m_dec );
        return ( neg ? -v : v ) * unit();
    }

    std::string token( const std::string& s, char aKey ) const
    {
        size_t at = s.find( aKey );

        if( at == std::string::npos )
            return std::string();

        size_t e = at + 1;

        while( e < s.size() && ( std::isdigit( (unsigned char) s[e] ) || s[e] == '-' || s[e] == '+' || s[e] == '.' ) )
            ++e;

        return s.substr( at + 1, e - at - 1 );
    }

    /// Point in the file's frame (mm, Y up) after the coordinates in aWords.
    Pt rawPoint( const std::string& w ) const
    {
        std::string x = token( w, 'X' ), y = token( w, 'Y' );
        Pt          p = m_cur;

        if( m_incremental )
        {
            if( !x.empty() )
                p.x += number( x );

            if( !y.empty() )
                p.y += number( y );
        }
        else
        {
            if( !x.empty() )
                p.x = number( x ) + m_origin.x;

            if( !y.empty() )
                p.y = number( y ) + m_origin.y;
        }

        return p;
    }

    Pt delta( const std::string& w ) const
    {
        std::string x = token( w, 'X' ), y = token( w, 'Y' );
        return Pt( x.empty() ? 0.0 : number( x ), y.empty() ? 0.0 : number( y ) );
    }

    double toolSize() const
    {
        auto it = m_tools.find( m_tool );
        return it == m_tools.end() ? 0.0 : it->second * unit();
    }

    void hit( const Pt& a, const Pt& b ) { hits.push_back( { toBoard( a.x, a.y ), toBoard( b.x, b.y ), toolSize() } ); }

    /// Routed arc from m_cur to aEnd, by radius (A) or centre offset (I, J): cut as short slots.
    void arcCut( const std::string& w, const Pt& end )
    {
        Pt     s = m_cur, c;
        bool   cw = m_arc == 2;
        double r;
        std::string a = token( w, 'A' );

        if( !a.empty() )
        {
            r = std::fabs( number( a ) );
            Pt     mid( ( s.x + end.x ) / 2, ( s.y + end.y ) / 2 );
            double half = std::hypot( end.x - s.x, end.y - s.y ) / 2;

            if( half < 1e-9 || r < half )
            {
                hit( s, end );
                return;
            }

            double h = std::sqrt( r * r - half * half );
            Pt     dir( ( end.x - s.x ) / ( 2 * half ), ( end.y - s.y ) / ( 2 * half ) );
            Pt     n = cw ? Pt( dir.y, -dir.x ) : Pt( -dir.y, dir.x ); // centre side for the short arc
            c = Pt( mid.x + n.x * h, mid.y + n.y * h );
        }
        else
        {
            Pt off = delta( std::string( "X" ) + token( w, 'I' ) + "Y" + token( w, 'J' ) );
            c = Pt( s.x + off.x, s.y + off.y );
            r = std::hypot( s.x - c.x, s.y - c.y );
        }

        double a0 = std::atan2( s.y - c.y, s.x - c.x ), a1 = std::atan2( end.y - c.y, end.x - c.x );
        bool   full = std::hypot( end.x - s.x, end.y - s.y ) < 1e-9;

        if( cw )
        {
            if( full )
                a1 = a0 - 2 * PI_;
            else
                while( a1 >= a0 )
                    a1 -= 2 * PI_;
        }
        else
        {
            if( full )
                a1 = a0 + 2 * PI_;
            else
                while( a1 <= a0 )
                    a1 += 2 * PI_;
        }

        Path pts;
        arcPoints( pts, c, r, a0, a1 );
        Pt prev = s;

        for( const Pt& q : pts )
        {
            hit( prev, q );
            prev = q;
        }
    }

    bool   m_mm = false; // Excellon defaults to inches
    int    m_lz = -1;    // 1 = LZ, 0 = TZ, -1 = not declared
    bool   m_lzGuess = false;
    bool   m_decimal = false;
    bool   m_routing = false, m_down = false;
    bool   m_formatGiven = false;
    bool   m_incremental = false;
    int    m_arc = 0;
    int    m_int = 2, m_dec = 4;
    int    m_tool = 0;
    Pt     m_cur, m_origin;
    std::map<int, double> m_tools;
};


/// "Copper,L2,Inr" etc. -> canonical layer name.
std::string layerFromFunction( const std::string& f, int copperCount )
{
    auto field = [&]( int k ) -> std::string
    {
        size_t s = 0;

        for( int i = 0; i < k; ++i )
        {
            s = f.find( ',', s );

            if( s == std::string::npos )
                return std::string();

            ++s;
        }

        size_t e = f.find( ',', s );
        return f.substr( s, e == std::string::npos ? std::string::npos : e - s );
    };

    std::string kind = field( 0 ), a = field( 1 ), b = field( 2 );
    bool        top = a == "Top" || b == "Top";

    if( kind == "Copper" && a.size() > 1 && a[0] == 'L' )
    {
        int n = std::atoi( a.c_str() + 1 );

        if( b == "Top" || n == 1 )
            return "F.Cu";

        if( b == "Bot" || ( copperCount > 0 && n == copperCount ) )
            return "B.Cu";

        return "In" + std::to_string( std::max( 1, n - 1 ) ) + ".Cu";
    }

    if( kind == "Soldermask" )
        return top ? "F.Mask" : "B.Mask";

    if( kind == "Legend" )
        return top ? "F.SilkS" : "B.SilkS";

    if( kind == "Paste" )
        return top ? "F.Paste" : "B.Paste";

    if( kind == "Profile" )
        return "Edge.Cuts";

    if( kind == "AssemblyDrawing" )
        return top ? "F.Fab" : "B.Fab";

    return std::string();
}

} // anonymous namespace


bool LooksLikeZip( const char* d, size_t n )
{
    return n >= 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4;
}


bool LooksLikeGerber( const char* d, size_t n )
{
    std::string head( d, std::min<size_t>( n, 4096 ) );
    return head.find( "%FS" ) != std::string::npos || ( head.find( "%MO" ) != std::string::npos && head.find( '*' ) != std::string::npos )
           || ( head.find( "G04" ) != std::string::npos && head.find( "%AD" ) != std::string::npos );
}


bool LooksLikeExcellon( const char* d, size_t n )
{
    std::string head( d, std::min<size_t>( n, 4096 ) );
    size_t      m48 = head.find( "M48" );

    if( m48 != std::string::npos && ( m48 == 0 || head[m48 - 1] == '\n' || head[m48 - 1] == '\r' ) )
        return true;

    // files without a header: no Gerber '*', a tool with a size and a hole position
    if( head.find( '*' ) != std::string::npos || head.find( '\0' ) != std::string::npos )
        return false;

    bool tool = false, hit = false;

    for( size_t i = 0; i + 1 < head.size(); ++i )
    {
        if( i > 0 && head[i - 1] != '\n' && head[i - 1] != '\r' )
            continue;

        if( head[i] == 'T' && std::isdigit( (unsigned char) head[i + 1] ) )
        {
            size_t e = head.find_first_of( "\r\n", i );
            tool |= head.substr( i, e == std::string::npos ? std::string::npos : e - i ).find( 'C' ) != std::string::npos;
        }

        if( ( head[i] == 'X' || head[i] == 'Y' ) && ( std::isdigit( (unsigned char) head[i + 1] ) || head[i + 1] == '-' ) )
            hit = true;
    }

    return tool && hit;
}


bool Unzip( const char* d, size_t n, std::vector<FabInput>& out, std::string& err )
{
    auto u16 = [&]( size_t p ) -> uint32_t { return p + 2 <= n ? (uint32_t) (uint8_t) d[p] | ( (uint32_t) (uint8_t) d[p + 1] << 8 ) : 0; };
    auto u32 = [&]( size_t p ) -> uint32_t { return u16( p ) | ( u16( p + 2 ) << 16 ); };

    // end of central directory: search backwards
    size_t eocd = std::string::npos;

    for( size_t p = n >= 22 ? n - 22 : 0; p + 4 <= n; --p )
    {
        if( u32( p ) == 0x06054b50 )
        {
            eocd = p;
            break;
        }

        if( p == 0 || n - p > 70000 )
            break;
    }

    if( eocd == std::string::npos )
    {
        err = Tr( "The zip archive is damaged" );
        return false;
    }

    uint32_t entries = u16( eocd + 10 );
    size_t   cd = u32( eocd + 16 );

    for( uint32_t k = 0; k < entries && cd + 46 <= n; ++k )
    {
        if( u32( cd ) != 0x02014b50 )
            break;

        uint32_t method = u16( cd + 10 );
        uint32_t csize = u32( cd + 20 ), usize = u32( cd + 24 );
        uint32_t nlen = u16( cd + 28 ), xlen = u16( cd + 30 ), clen = u16( cd + 32 );
        size_t   local = u32( cd + 42 );
        std::string name( d + cd + 46, std::min<size_t>( nlen, n - cd - 46 ) );
        cd += 46 + nlen + xlen + clen;

        if( name.empty() || name.back() == '/' || local + 30 > n || u32( local ) != 0x04034b50 )
            continue;

        size_t data = local + 30 + u16( local + 26 ) + u16( local + 28 );

        if( data + csize > n || usize > 256u * 1024 * 1024 )
            continue;

        std::string content;

        if( method == 0 )
        {
            content.assign( d + data, csize );
        }
        else if( method == 8 )
        {
            content.resize( usize );
            z_stream z {};

            if( inflateInit2( &z, -MAX_WBITS ) != Z_OK )
                continue;

            z.next_in = (Bytef*) ( d + data );
            z.avail_in = csize;
            z.next_out = (Bytef*) &content[0];
            z.avail_out = usize;
            int rc = inflate( &z, Z_FINISH );
            inflateEnd( &z );

            if( rc != Z_STREAM_END )
                continue;
        }
        else
        {
            continue;
        }

        size_t slash = name.find_last_of( '/' );
        out.push_back( { slash == std::string::npos ? name : name.substr( slash + 1 ), std::move( content ) } );
    }

    return true;
}


std::string GerberLayerFor( const std::string& aName, const std::string& aFunction )
{
    if( !aFunction.empty() )
    {
        std::string l = layerFromFunction( aFunction, 0 );

        if( !l.empty() )
            return l;
    }

    std::string n = lower( aName );

    // KiCad names: board-F_Cu.gbr, board-In2_Cu.gbr, board-Edge_Cuts.gm1 ...
    static const std::pair<const char*, const char*> KICAD[] = {
        { "-f_cu.", "F.Cu" },         { "-b_cu.", "B.Cu" },           { "-f_mask.", "F.Mask" },
        { "-b_mask.", "B.Mask" },     { "-f_paste.", "F.Paste" },     { "-b_paste.", "B.Paste" },
        { "-f_silkscreen.", "F.SilkS" }, { "-b_silkscreen.", "B.SilkS" }, { "-f_silks.", "F.SilkS" },
        { "-b_silks.", "B.SilkS" },   { "-edge_cuts.", "Edge.Cuts" }, { "-f_fab.", "F.Fab" },
        { "-b_fab.", "B.Fab" },       { "-f_courtyard.", "F.CrtYd" }, { "-b_courtyard.", "B.CrtYd" }
    };

    for( const auto& [k, v] : KICAD )
    {
        if( n.find( k ) != std::string::npos )
            return v;
    }

    size_t in = n.find( "-in" );

    if( in != std::string::npos && n.find( "_cu.", in ) != std::string::npos )
    {
        int k = std::atoi( n.c_str() + in + 3 );

        if( k > 0 )
            return "In" + std::to_string( k ) + ".Cu";
    }

    // Protel / Altium extensions
    size_t      dot = n.find_last_of( '.' );
    std::string ext = dot == std::string::npos ? std::string() : n.substr( dot + 1 );
    static const std::pair<const char*, const char*> EXT[] = {
        { "gtl", "F.Cu" },   { "gbl", "B.Cu" },      { "gts", "F.Mask" },    { "gbs", "B.Mask" },
        { "gtp", "F.Paste" }, { "gbp", "B.Paste" },  { "gto", "F.SilkS" },   { "gbo", "B.SilkS" },
        { "gko", "Edge.Cuts" }, { "gm1", "Edge.Cuts" }, { "gml", "Edge.Cuts" }, { "gm", "Edge.Cuts" },
        { "cmp", "F.Cu" },   { "sol", "B.Cu" },      { "stc", "F.Mask" },    { "sts", "B.Mask" },
        { "plc", "F.SilkS" }, { "pls", "B.SilkS" },  { "crc", "F.Paste" },   { "crs", "B.Paste" }
    };

    for( const auto& [k, v] : EXT )
    {
        if( ext == k )
            return v;
    }

    // inner layers: .g1 .. .g30 and .gp1 ..
    if( ext.size() >= 2 && ext[0] == 'g' && std::isdigit( (unsigned char) ext[ext.size() - 1] ) )
    {
        size_t d0 = ext.find_first_of( "0123456789" );
        int    k = std::atoi( ext.c_str() + d0 );

        if( k >= 1 && k <= 30 && ( d0 == 1 || ( d0 == 2 && ext[1] == 'p' ) ) )
            return "In" + std::to_string( k ) + ".Cu";
    }

    return std::string();
}


void LoadFabFiles( Board& b, const std::vector<FabInput>& files0 )
{
    b = Board();
    b.nets.push_back( std::string() );
    b.generator = "gerber";

    std::vector<FabInput> unpacked;

    for( const FabInput& f : files0 )
    {
        if( LooksLikeZip( f.data.data(), f.data.size() ) )
        {
            std::string err;

            if( !Unzip( f.data.data(), f.data.size(), unpacked, err ) )
                b.warnings.push_back( f.name + ": " + err );
        }
        else
        {
            unpacked.push_back( f );
        }
    }

    // the same file picked twice, or loose files next to their zip, is read once
    std::vector<FabInput> files;
    std::vector<std::string> seen;

    for( FabInput& f : unpacked )
    {
        std::string key = lower( f.name );

        if( std::find( seen.begin(), seen.end(), key ) != seen.end() )
            continue;

        seen.push_back( key );
        files.push_back( std::move( f ) );
    }

    auto layerIndex = [&]( const std::string& canonical ) -> int
    {
        for( size_t i = 0; i < b.layers.size(); ++i )
        {
            if( b.layers[i].canonical == canonical )
                return (int) i;
        }

        Layer L;

        if( !DescribeLayer( canonical, L ) )
            return -1;

        b.layers.push_back( L );
        return (int) b.layers.size() - 1;
    };

    int userLayers = 0, read = 0;
    for( const FabInput& f : files )
    {
        const char* d = f.data.data();
        size_t      n = f.data.size();
        std::string ln = lower( f.name );

        if( LooksLikeExcellon( d, n ) )
        {
            ExcellonReader r;
            r.Parse( f.data );
            bool plated = ln.find( "npth" ) == std::string::npos && ln.find( "-np" ) == std::string::npos;

            for( const DrillHit& h : r.hits )
            {
                if( h.dia <= 0 )
                    continue;

                Pad p;
                p.number = "";
                p.attr = plated ? PadAttr::PTH : PadAttr::NPTH;
                p.geom.shape = PadShape::CIRCLE;
                p.geom.size = Pt( h.dia, h.dia );
                Pt     d2 = h.b - h.a;
                double len = std::hypot( d2.x, d2.y );

                if( len > 1e-6 )
                {
                    p.pos = ( h.a + h.b ) * 0.5;
                    p.angle = std::atan2( -d2.y, d2.x ) * 180 / PI_;
                    p.drill = Pt( len + h.dia, h.dia );
                    p.drillOval = true;
                    p.geom.shape = PadShape::OVAL;
                    p.geom.size = p.drill;
                }
                else
                {
                    p.pos = h.a;
                    p.drill = Pt( h.dia, h.dia );
                }

                b.pads.push_back( p );
            }

            ++read;
            continue;
        }

        if( !LooksLikeGerber( d, n ) )
        {
            // job files, reports, READMEs ... are fine to skip quietly
            if( !endsWith( ln, ".gbrjob" ) && !endsWith( ln, ".txt" ) && !endsWith( ln, ".csv" ) && !endsWith( ln, ".pdf" )
                && !endsWith( ln, ".rpt" ) && !endsWith( ln, ".md" ) )
                b.warnings.push_back( TrF( "Not a Gerber or drill file: {0}", { f.name } ) );

            continue;
        }

        // the X2 file function decides the layer when present
        // (also as a "G04 #@! TF..." comment, which KiCad writes when X2 is turned off)
        std::string function;

        for( const char* tag : { "%TF.FileFunction,", "#@! TF.FileFunction," } )
        {
            size_t ff = f.data.find( tag );

            if( ff != std::string::npos )
            {
                size_t from = ff + std::strlen( tag );
                size_t e = f.data.find( '*', from );
                function = f.data.substr( from, e == std::string::npos ? 0 : e - from );
                break;
            }
        }

        std::string canonical = GerberLayerFor( f.name, function );

        if( canonical.empty() )
            canonical = "User." + std::to_string( std::min( 9, ++userLayers ) );

        int layer = layerIndex( canonical );

        if( layer < 0 )
            continue;

        GerberReader r( b, layer );
        r.Parse( f.data );
        ++read;
    }

    if( read == 0 )
        throw ParseError( Tr( "No Gerber or drill files found" ), 0 );


    // copper layers give the stack (at least F.Cu and B.Cu for drill hits)
    if( !b.pads.empty() )
    {
        layerIndex( "F.Cu" );
        layerIndex( "B.Cu" );
    }

    FinishLayerStack( b );

    for( size_t i = 0; i < b.layers.size(); ++i )
        b.layerByName[b.layers[i].canonical] = (int) i;

    b.Finalize();
}

} // namespace kp
