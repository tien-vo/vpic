#define IN_spa
#include "spa_private.h"

#define f(x,y,z) f[INDEX_FORTRAN_3(x,y,z,0,g->nx+1,0,g->ny+1,0,g->nz+1)]

// Accumulate particle to rhob with locally adjusted accumulation.
// FIXME: THIS FUNCTION DOESN'T BELONG HERE ANYMORE!

void
accumulate_rhob( field_t          * __restrict__ ALIGNED(128) f,
                 const particle_t * __restrict__ ALIGNED(32)  p,
                 const grid_t     * __restrict__              g ) {
  float w0, w1, w2, w3, w4, w5, w6, w7, t;
  int i, j, k;
  float *rhob;

  // Compute the trilinear weights
  t   = p->dx;                      // t  = x
  w0  = 0.125*p->q*g->rdx*g->rdy*g->rdz; // w0 = w/8
  t  *= w0;                         // t  = wx/8
  w1  = w0+t;                       // w1 = w/8 + wx/8 = (w/8)(1+x)
  w0 -= t;                          // w0 = w/8 - wx/8 = (w/8)(1-x)
  t   = p->dy;                      // t  = y
  w3  = 1+t;                        // w3 = 1+y
  w2  = w0*w3;                      // w2 = (w/8)(1-x)(1+y)
  w3 *= w1;                         // w3 = (w/8)(1+x)(1+y)
  t   = 1-t;                        // t  = 1-y
  w0 *= t;                          // w0 = (w/8)(1-x)(1-y)
  w1 *= t;                          // w1 = (w/8)(1+x)(1-y)
  t   = p->dz;                      // t  = z
  w7  = 1+t;                        // w7 = 1+z
  w4  = w0*w7;                      // w4 = (w/8)(1-x)(1-y)(1+z) *Done
  w5  = w1*w7;                      // w5 = (w/8)(1+x)(1-y)(1+z) *Done
  w6  = w2*w7;                      // w6 = (w/8)(1-x)(1+y)(1+z) *Done
  w7 *= w3;                         // w7 = (w/8)(1+x)(1+y)(1+z) *Done
  t   = 1-t;                        // t  = 1-z
  w0 *= t;                          // w0 = (w/8)(1-x)(1-y)(1-z) *Done
  w1 *= t;                          // w1 = (w/8)(1+x)(1-y)(1-z) *Done
  w2 *= t;                          // w2 = (w/8)(1-x)(1+y)(1-z) *Done
  w3 *= t;                          // w3 = (w/8)(1+x)(1+y)(1-z) *Done
  
  // Adjust the weights for a corrected local accumulation of rhob
  // See note in synchronize_rho why we must do this for rhob and
  // not for rhof.

  i  = p->i;        // i = INDEX_FORTRAN_3(ix,iy,iz,0,nx+1,0,ny+1,0,nz+1)
  /**/              //   = ix + (nx+2)*( iy + (ny+2)*iz )
  j  = i/(g->nx+2); // j = iy + (ny+2)*iz
  i -= j*(g->nx+2); // i = ix
  k  = j/(g->ny+2); // k = iz
  j -= k*(g->ny+2); // j = iy
  if( i==1     ) w0 += w0, w2 += w2, w4 += w4, w6 += w6;
  if( i==g->nx ) w1 += w1, w3 += w3, w5 += w5, w7 += w7;
  if( j==1     ) w0 += w0, w1 += w1, w4 += w4, w5 += w5;
  if( j==g->ny ) w2 += w2, w3 += w3, w6 += w6, w7 += w7;
  if( k==1     ) w0 += w0, w1 += w1, w2 += w2, w3 += w3;
  if( k==g->nz ) w4 += w4, w5 += w5, w6 += w6, w7 += w7;
  
  // Update rhob
  i = &f(1,0,0).rhob - &f(0,0,0).rhob;
  j = &f(0,1,0).rhob - &f(1,0,0).rhob;
  k = &f(0,0,1).rhob - &f(1,1,0).rhob;
  rhob = &f[p->i].rhob; *rhob += w0;
  rhob += i;            *rhob += w1;
  rhob += j;            *rhob += w2;
  rhob += i;            *rhob += w3;
  rhob += k;            *rhob += w4;
  rhob += i;            *rhob += w5;
  rhob += j;            *rhob += w6;
  rhob += i;            *rhob += w7;
}

// FIXME: ARCHITECTURAL FLAW!  CUSTOM BCS AND SHARED FACES CANNOT
// COEXIST ON THE SAME FACE!  THIS MEANS THAT CUSTOM BOUNDARYS MUST
// REINJECT ALL ABSORBED PARTICLES IN THE SAME DOMAIN!

void
boundary_p( species_t        * __restrict__ sp_list,
            field_t          * __restrict__ ALIGNED(128) f,
            accumulator_t    * __restrict__ ALIGNED(128) a0,
            const grid_t     * __restrict__ g,
            mt_rng_t         *              rng ) {
  const int sf2b[6] = { BOUNDARY(-1, 0, 0),
                        BOUNDARY( 0,-1, 0),
                        BOUNDARY( 0, 0,-1),
                        BOUNDARY( 1, 0, 0),
                        BOUNDARY( 0, 1, 0),
                        BOUNDARY( 0, 0, 1) };

  const int rf2b[6] = { BOUNDARY( 1, 0, 0),
                        BOUNDARY( 0, 1, 0),
                        BOUNDARY( 0, 0, 1),
                        BOUNDARY(-1, 0, 0),
                        BOUNDARY( 0,-1, 0),
                        BOUNDARY( 0, 0,-1) };

  const int rank  = mp_rank(g->mp);
  const int nproc = mp_nproc(g->mp);

# define SHARED_REMOTELY(bound) \
  ((g->bc[bound]>=0) & (g->bc[bound]<nproc) & (g->bc[bound]!=rank))

  const int64_t * __restrict__ ALIGNED(128) neighbor = g->neighbor;
  const int rangel = g->rangel;
  const int rangeh = g->rangeh;
  const int rangem = g->range[nproc];
  const int range0 = SHARED_REMOTELY(sf2b[0]) ? g->range[g->bc[sf2b[0]]] : 0;
  const int range1 = SHARED_REMOTELY(sf2b[1]) ? g->range[g->bc[sf2b[1]]] : 0;
  const int range2 = SHARED_REMOTELY(sf2b[2]) ? g->range[g->bc[sf2b[2]]] : 0;
  const int range3 = SHARED_REMOTELY(sf2b[3]) ? g->range[g->bc[sf2b[3]]] : 0;
  const int range4 = SHARED_REMOTELY(sf2b[4]) ? g->range[g->bc[sf2b[4]]] : 0;
  const int range5 = SHARED_REMOTELY(sf2b[5]) ? g->range[g->bc[sf2b[5]]] : 0;

  boundary_t * __restrict__ boundary = g->boundary;
  const int                 nb       = g->nb; 

  species_t * __restrict__ sp;
  int face, ns[6], ncm;

  static particle_injector_t * __restrict__ ALIGNED(16) cmlist = NULL;
  static int cmlist_size = 0;

  // Presize various buffers
  //
  // Each buffer is large enough to hold one injector corresponding
  // to every mover in use (worst case, but plausible scenario in
  // beam simulations, is one buffer gets all the movers).
  //
  // FIXME: THIS IS ASSUMES THAT CUSTOM BOUNDARY CONDITIONS INJECT
  // AT MOST ONE PARTICLE PER INCIDENT PARTICLE.  THIS IS USUALLY
  // TRUE (SOME SECONDARY MODELS MAY NOT SATISFY) BUT THIS
  // ARCHITECTURAL FLAW SHOULD BE FIXED.
  //
  // FIXME: WE COULD BE ~7X MORE EFFICIENT IN OUR PARTICLE INJECTOR
  // SIZING HERE.  CREATE ON LOCAL INJECTOR BUFFER OF NM IN SIZE.
  // THEN WE WOULD DO INJECTION FOR ALL LOCAL AND REMOTE BOUNDARIES
  // TO THIS BUFFER.  IN-PLACE STORE THE BUFFER, PRESERVING THE
  // PARTTIONING.  LOAD UP MESSAGING APPROPRIATELY.  REQUIRES SOME
  // MP REDESIGN!

  do {
    int nm = 0; LIST_FOR_EACH( sp, sp_list ) nm += sp->nm;
    
    for( face=0; face<6; face++ )
      if( SHARED_REMOTELY(sf2b[face] ) )
        mp_size_send_buffer( sf2b[face],
                             16 + nm*sizeof(particle_injector_t),
                             g->mp );
    
    if( cmlist_size<nm ) {
      particle_injector_t * tmp = cmlist; // Hack around __restrict__
      FREE_ALIGNED( tmp );
      MALLOC_ALIGNED( tmp, nm, 16 );
      cmlist      = tmp;
      cmlist_size = nm;
    }
  } while(0);
  
  // Load the particle send buffers and the local particle injection
  // buffer.  Note: particle movers for each species are processed
  // in reverse order.  This allows us to backfill holes in the
  // particle list created by boundary conditions and/or
  // communication.  This assumes particles on the mover list are
  // monotonically increasing.  That is: pm[n].i > pm[n-1].i for
  // n=1...nm-1.  advance_p and inject_particle create movers with
  // property if all aged particle injection occurs after advance_p
  // and before this
  
  do {
    particle_injector_t * __restrict__ ALIGNED(16) ps0 =
      (particle_injector_t *)( ((char *)mp_send_buffer(sf2b[0],g->mp))+16 );
    particle_injector_t * __restrict__ ALIGNED(16) ps1 =
      (particle_injector_t *)( ((char *)mp_send_buffer(sf2b[1],g->mp))+16 );
    particle_injector_t * __restrict__ ALIGNED(16) ps2 =
      (particle_injector_t *)( ((char *)mp_send_buffer(sf2b[2],g->mp))+16 );
    particle_injector_t * __restrict__ ALIGNED(16) ps3 =
      (particle_injector_t *)( ((char *)mp_send_buffer(sf2b[3],g->mp))+16 );
    particle_injector_t * __restrict__ ALIGNED(16) ps4 =
      (particle_injector_t *)( ((char *)mp_send_buffer(sf2b[4],g->mp))+16 );
    particle_injector_t * __restrict__ ALIGNED(16) ps5 =
      (particle_injector_t *)( ((char *)mp_send_buffer(sf2b[5],g->mp))+16 );
    particle_injector_t * /*__restrict__*/ ALIGNED(16) cm  = cmlist;
    int ns0 = 0, ns1 = 0, ns2 = 0, ns3 = 0, ns4 = 0, ns5 = 0;
    
    LIST_FOR_EACH( sp, sp_list ) {
      particle_t       * __restrict__ ALIGNED(128) p0 = sp->p;
      particle_mover_t * __restrict__ ALIGNED(16)  pm = sp->pm + sp->nm - 1;
      const int32_t sp_id = sp->id;
      int np = sp->np;
      int nm = sp->nm;
      
      for( ; nm; pm--, nm-- ) {
        particle_t * __restrict__ ALIGNED(32) r = p0 + pm->i;
        float   dx = r->dx;
        float   dy = r->dy;
        float   dz = r->dz;
        int32_t i  = r->i;
        float   ux = r->ux;
        float   uy = r->uy;
        float   uz = r->uz;
        float   q  = r->q;
        
        // FIXME: HAVE SPE HANDLER INDICATE THE FACE IN THE MOVER
        // E.G. LIKE pm->i == 8*cell + FACE.  THIS ELIMINATES THE
        // NEED TO TEST FACES FOR INTERACTION AT ALL!
        
        // FIXME: BOUNDARY_HANDLERS SHOULD RETURN THE NUMBER OF
        // INJECTORS THEY USED.  CM CAN THEN BE ELIMINATED TOO.
        
        // FIXME: Currently, after a particle interacts with a
        // boundary it is removed from the local particle list.
        // Thus, if a boundary handler does not want a particle
        // destroyed, it is the boundary handler's job to append the
        // destroyed particle to the list of particles to inject.
        //
        // Note that these destructing and creation processes do
        // _not_ adjust rhob by default.  Thus, a boundary handler
        // is responsible for insuring that the rhob is updated
        // appropriate for the incident particle it destroys and for
        // any particles it as a result too.
        //
        // Since most boundary handlers do local reinjection and are
        // charge neutral, this means most boundary handlers do
        // nothing to rhob.
        //
        // In the future, the boundary handlers should be adjusted
        // to determine whether or not the interacting particle
        // should be removed in addition to keeping the charge
        // conservation books.
        
#       define TEST_FACE(FACE,cond)                                     \
        if(cond) {                                                      \
          int64_t nn = neighbor[ 6*i + FACE ];                          \
          if( nn==absorb_particles ) {                                  \
            accumulate_rhob( f, r, g );                                 \
            r[0] = p0[--np];                                            \
            continue;                                                   \
          }                                                             \
          if( ((nn>=0)     & (nn< rangel)) |                            \
              ((nn>rangeh) & (nn<=rangem)) )  {                         \
            /* nn - range... is less than 2^31 / 6 */                   \
            ps##FACE[ns##FACE].dx    = ((FACE==0) | (FACE==3)) ? -dx : dx; \
            ps##FACE[ns##FACE].dy    = ((FACE==1) | (FACE==4)) ? -dy : dy; \
            ps##FACE[ns##FACE].dz    = ((FACE==2) | (FACE==5)) ? -dz : dz; \
            ps##FACE[ns##FACE].i     = nn - range##FACE;                \
            ps##FACE[ns##FACE].ux    = ux;                              \
            ps##FACE[ns##FACE].uy    = uy;                              \
            ps##FACE[ns##FACE].uz    = uz;                              \
            ps##FACE[ns##FACE].q     = q;                               \
            ps##FACE[ns##FACE].dispx = pm->dispx;                       \
            ps##FACE[ns##FACE].dispy = pm->dispy;                       \
            ps##FACE[ns##FACE].dispz = pm->dispz;                       \
            ps##FACE[ns##FACE].sp_id = sp_id;                           \
            ns##FACE++;                                                 \
            r[0] = p0[--np];                                            \
            continue;                                                   \
          }                                                             \
                                                                        \
          /* Test for user-defined boundary handler.          */        \
          /* Note: Particle is destroyed after it is handled. */        \
                                                                        \
          nn = -nn - 3; /* Assumes reflecting/absorbing are -1, -2 */   \
          if( (nn>=0) & (nn<nb) ) {                                     \
            boundary[nn].handler( boundary[nn].params,                  \
                                  r, pm, f, a0, g, sp, &cm, rng, FACE ); \
            r[0] = p0[--np];                                            \
            continue;                                                   \
          }                                                             \
        }
        
        TEST_FACE(0,(dx==-1) & (ux<0));
        TEST_FACE(1,(dy==-1) & (uy<0));
        TEST_FACE(2,(dz==-1) & (uz<0));
        TEST_FACE(3,(dx== 1) & (ux>0));
        TEST_FACE(4,(dy== 1) & (uy>0));
        TEST_FACE(5,(dz== 1) & (uz>0));
#       undef TEST_FACE
        
        WARNING(( "Unknown boundary interaction ... using absorption "
                  "(species=%s, rank=%i)", sp->name, rank ));
        accumulate_rhob( f, r, g );
        r[0] = p0[--np];
      }
      
      sp->np = np;
      sp->nm = 0;
    }

    ns[0] = ns0;
    ns[1] = ns1;
    ns[2] = ns2;
    ns[3] = ns3;
    ns[4] = ns4;
    ns[5] = ns5;
    ncm   = cm - cmlist;
  } while(0);

  // Exchange particle counts
  
  // FIXME: WASTEFULL OF COMMUNICATIONS HERE.  COULD AT LEAST NOT DO
  // THE SECOND COMMUNICATION IF NO DATA TO SEND (BOTH ENDS KNOW
  // THIS!)  BETTER WOULD BE TO USE EITHER PRESIZED BUFFERS OR A
  // STATEFUL PROTOCOL TO PERMIT ONE MESSAGE PER FACE TO BE SENT
  // RATHER THAN TWO!
  
  // FIXME: ARE MP_SIZES HERE REALLY NECESSARY??
  
  for( face=0; face<6; face++ )
    if( SHARED_REMOTELY(sf2b[face]) ) {
      *((int *)mp_send_buffer( sf2b[face], g->mp )) = ns[face];
      mp_begin_send( sf2b[face],
                     sizeof(int),
                     g->bc[sf2b[face]],
                     sf2b[face],
                     g->mp );
    }
  
  for( face=0; face<6; face++ )
    if( SHARED_REMOTELY(rf2b[face]) ) {
      mp_size_recv_buffer( rf2b[face], sizeof(int), g->mp );
      mp_begin_recv( rf2b[face],
                     sizeof(int),
                     g->bc[rf2b[face]],
                     sf2b[face],
                     g->mp );
    }
  
  for( face=0; face<6; face++ )
    if( SHARED_REMOTELY(rf2b[face]) ) mp_end_recv( rf2b[face], g->mp );
  
  for( face=0; face<6; face++ )
    if( SHARED_REMOTELY(sf2b[face]) ) mp_end_send( sf2b[face], g->mp );
  
  // Exchange particles
  
  for( face=0; face<6; face++ )
    if( SHARED_REMOTELY(sf2b[face]) )
      mp_begin_send( sf2b[face],
                     16 + ns[face]*sizeof(particle_injector_t),
                     g->bc[sf2b[face]], sf2b[face], g->mp );
  
  for( face=0; face<6; face++ )
    if( SHARED_REMOTELY(rf2b[face]) ) {
      int sz = *((int *)mp_recv_buffer( rf2b[face], g->mp ));
      sz = 16 + sz*sizeof(particle_injector_t);
      mp_size_recv_buffer( rf2b[face], sz, g->mp );
      mp_begin_recv( rf2b[face], sz, g->bc[rf2b[face]], sf2b[face], g->mp );
    }
  
  for( face=0; face<6; face++ )
    if( SHARED_REMOTELY(rf2b[face]) ) mp_end_recv( rf2b[face], g->mp );
  
  // Inject received particles
  
  do {
    particle_injector_t * __restrict__ ALIGNED(16) pi;
    species_t * sp_table[ 64 ];
    int n, n_inj[ 64 ];
    
    // Count the number of particles each species will inject
    
    LIST_FOR_EACH( sp, sp_list ) {
      if( sp->id<0 || sp->id>=64 ) ERROR(( "Invalid sp->id" ));
      sp_table[ sp->id ] = sp;
      n_inj[    sp->id ] = 0;
    }
    
    for( face=0; face<7; face++ ) {
      if( face==6 ) pi = cmlist, n = ncm;
      else if( !SHARED_REMOTELY(rf2b[face]) ) continue;
      else {
        char * buf = (char *)mp_recv_buffer( rf2b[face], g->mp );
        pi = (particle_injector_t *)( buf + 16 );
        n  = *((int *)buf);
      }
      
      for( ; n; pi++, n-- ) n_inj[ pi->sp_id ]++;
    }
    
    // Resize each species's particle and mover storage to be large
    // enough to guarantee successful injection if necessary
    
    LIST_FOR_EACH( sp, sp_list ) {
      n = sp->np + n_inj[sp->id];
      if( n>sp->max_np ) {
        particle_t * new_p;
        n = n + (n>>2) + (n>>4); // Increase by 31.25% (~<"silver
                                 // ratio") to minimize resizes (max
                                 // rate that avoids excessive heap
                                 // fragmentation)
        WARNING(( "Resizing local %s particle storage from %i to %i",
                  sp->name, sp->max_np, n ));
        MALLOC_ALIGNED( new_p, n, 128 );
        COPY( new_p, sp->p, sp->np );
        FREE_ALIGNED( sp->p );
        sp->p      = new_p;
        sp->max_np = n;
      }
      
      n = sp->nm + n_inj[sp->id];
      if( n>sp->max_nm ) {
        particle_mover_t * new_pm;
        n = n + (n>>2) + (n>>4); // Increase by 31.25% (~<"silver
                                 // ratio") to minimize resizes (max
                                 // rate that avoids excessive heap
                                 // fragmentation)
        WARNING(( "Resizing local %s mover storage from %i to %i",
                  sp->name, sp->max_nm, n ));
        MALLOC_ALIGNED( new_pm, n, 128 );
        COPY( new_pm, sp->pm, sp->nm );
        FREE_ALIGNED( sp->pm );
        sp->pm     = new_pm;
        sp->max_nm = n;
      }
    }
    
    // Inject particles
    
    // FIXME: THERE IS _NO_ REASON THAT ALL THE INJECTOR BUFFERS
    // COULDN'T BE MERGED INTO A NICE BIG ARRAY OF SINGLE BUFFER OF
    // INJECTORS.  SEE NOTE ABOVE ABOUT BEING MORE EFFICIENT WITH
    // MEMORY HERE!

    for( face=0; face<7; face++ ) {
      if( face==6 ) pi = cmlist, n = ncm;
      else if( !SHARED_REMOTELY(rf2b[face]) ) continue;
      else {
        char * buf = (char *)mp_recv_buffer( rf2b[face], g->mp );
        pi = (particle_injector_t *)( buf + 16 );
        n     = *((int *)buf);
      }
      
      // Reverse order injection is done to reduce thrashing of the
      // particle list (particles are removed reverse order so the
      // overall impact of removal + injection is to keep injected
      // particles in order).

      // WARNING: THIS TRUSTS THAT THE INJECTORS (INCLUDING THOSE
      // RECEIVED FROM OTHER NODES) HAVE VALID PARTICLE IDS.

      pi += n-1;
      for( ; n; pi--, n-- ) {
        sp = sp_table[pi->sp_id];
        sp->p[ sp->np]   = *(particle_t       *)(&(pi->dx   ));
        sp->pm[sp->nm]   = *(particle_mover_t *)(&(pi->dispx));
        sp->pm[sp->nm].i = sp->np;
        sp->np++;
        sp->nm += move_p( sp->p, &sp->pm[sp->nm], a0, g );
      }
    }
    
  } while(0);
  
  // Communication wrap up
  
  for( face=0; face<6; face++ )
    if( SHARED_REMOTELY(sf2b[face]) ) mp_end_send( sf2b[face], g->mp );
}
