/***************************************************************************
*   Copyright 2012 - 2013 Advanced Micro Devices, Inc.
*
*   Licensed under the Apache License, Version 2.0 (the "License");
*   you may not use this file except in compliance with the License.
*   You may obtain a copy of the License at
*
*       http://www.apache.org/licenses/LICENSE-2.0
*
*   Unless required by applicable law or agreed to in writing, software
*   distributed under the License is distributed on an "AS IS" BASIS,
*   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*   See the License for the specific language governing permissions and
*   limitations under the License.

***************************************************************************/


#if !defined( BOLT_AMP_MERGE_INL )
#define BOLT_AMP_MERGE_INL
#pragma once

#include <algorithm>

#include "bolt/amp/bolt.h"
#include "bolt/amp/functional.h"
#include "bolt/amp/device_vector.h"

#ifdef ENABLE_TBB
//TBB Includes
#include "bolt/btbb/merge.h"
#endif


namespace bolt {
    namespace amp {

        namespace detail {

			template< typename iTypePtr1, typename iTypeIter1, typename StrictWeakCompare >
			int binary_search1(iTypePtr1 value, iTypeIter1  a, int start, int length, const  StrictWeakCompare &userFunctor)   restrict(amp)
			{
				int low = start;
				int high = length;
				while (low < high)
				{
					long mid = (low + high) / 2;
					iTypePtr1 val1 = a[mid];
					if ((userFunctor)(val1, value))
						low = mid + 1;
					else
						high = mid;

				}
				return high;
			}

			template< typename iTypePtr1, typename iTypeIter1, typename StrictWeakCompare >
			int binary_search2(iTypePtr1 value, iTypeIter1  a, int start, int length, const StrictWeakCompare &userFunctor)   restrict(amp)
			{
				int low = start;
				int high = length;
				while (low < high)
				{
					long mid = (low + high) / 2;
					iTypePtr1 val1 = a[mid];
					if ((userFunctor)(value, val1))
						high = mid;
					else
						low = mid + 1;

				}
				return high;
			}



    
            //----
            // This is the base implementation of reduction that is called by all of the convenience wrappers below.
            // first and last must be iterators from a DeviceVector

            template<typename DVInputIterator1,typename DVInputIterator2,typename DVOutputIterator, 
            typename StrictWeakCompare>
            DVOutputIterator  merge_enqueue(bolt::amp::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                const DVOutputIterator& result,
                const StrictWeakCompare& comp )
            {
                typedef typename std::iterator_traits< DVInputIterator1 >::value_type iType1;
                typedef typename std::iterator_traits< DVInputIterator2 >::value_type iType2;
                typedef typename std::iterator_traits< DVOutputIterator >::value_type rType;
                
				const unsigned int length1 = static_cast< unsigned int >(std::distance(first1, last1));
				const unsigned int length2 = static_cast< unsigned int >(std::distance(first2, last2));

		
				concurrency::array_view<iType1, 1> inputV1(first1.getContainer().getBuffer());
				concurrency::array_view<iType2, 1> inputV2(first2.getContainer().getBuffer());
				concurrency::array_view<rType, 1> resultV(result.getContainer().getBuffer());
				int wgSize = 64;


				int leng = length1 > length2 ? length1 : length2;
				leng = leng + wgSize - (leng % wgSize);

				concurrency::extent< 1 > inputExtent(leng);


				concurrency::parallel_for_each(ctl.getAccelerator().default_view, inputExtent, [=](concurrency::index<1> idx) mutable restrict(amp)
				{
					
					int gx = idx[0];
					int pos1, pos2;

					if (gx < length1)
					{
						iType1 val = inputV1[gx];
						pos1 = binary_search1<iType1, concurrency::array_view<iType1, 1>,  StrictWeakCompare > (val, inputV2, 0, length2, comp);
						if ((inputV2[pos1 - 1] == val)  && pos1 != 0)
							resultV[pos1 + gx - 1] = val;
						else
							resultV[pos1 + gx] = val;

					}

					if (gx < length2)
					{
						iType2 val = inputV2[gx];
						pos2 = binary_search2<iType1, concurrency::array_view<iType1, 1>, StrictWeakCompare >(val, inputV1, 0, length1, comp);
						resultV[pos2 + gx] = val;
					}
				});

				return result + (last1 - first1) + (last2 - first2);;

            }
  

            // This template is called after we detect random access iterators
            // This is called strictly for any non-device_vector iterator
            template<typename InputIterator1,typename InputIterator2,typename OutputIterator, 
            typename StrictWeakCompare>
            OutputIterator merge_pick_iterator(bolt::amp::control &ctl,
                const InputIterator1& first1,
                const InputIterator1& last1,
                const InputIterator2& first2,
                const InputIterator2& last2,
                const OutputIterator& result,
                const StrictWeakCompare& comp,
                std::random_access_iterator_tag )
            {
                /*************/
                typedef typename std::iterator_traits<InputIterator1>::value_type iType1;
                typedef typename std::iterator_traits<InputIterator2>::value_type iType2;
                typedef typename std::iterator_traits<OutputIterator>::value_type oType;


                /*TODO - probably the forceRunMode should be replaced by getRunMode and setRunMode*/
                bolt::amp::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.

                if(runMode == bolt::amp::control::Automatic)
                {
					runMode = ctl.getForceRunMode();
                }

				#if defined(BOLT_DEBUG_LOG)
                BOLTLOG::CaptureLog *dblog = BOLTLOG::CaptureLog::getInstance();
                #endif
				
                switch(runMode)
                {
                case bolt::amp::control::Gpu :
                    {
					  #if defined(BOLT_DEBUG_LOG)
                      dblog->CodePathTaken(BOLTLOG::BOLT_MERGE,BOLTLOG::BOLT_GPU,"::Merge::GPU");
                      #endif
                      size_t sz = (last1-first1) + (last2-first2);
                      device_vector< iType1 > dvInput1( first1, last1, ctl );
                      device_vector< iType2 > dvInput2( first2, last2, ctl );
                      device_vector< oType >  dvresult(  result, sz, ctl );

                       detail::merge_enqueue( ctl, dvInput1.begin(), dvInput1.end(), dvInput2.begin(), dvInput2.end(),
                          dvresult.begin(), comp);

                        // This should immediately map/unmap the buffer
                        dvresult.data( );
                        return result + (last1 - first1) + (last2 - first2);
                    }
              
                case bolt::amp::control::MultiCoreCpu: 
                    #ifdef ENABLE_TBB
					    #if defined(BOLT_DEBUG_LOG)
                        dblog->CodePathTaken(BOLTLOG::BOLT_MERGE,BOLTLOG::BOLT_MULTICORE_CPU,"::Merge::MULTICORE_CPU");
                        #endif
                        return bolt::btbb::merge(first1,last1,first2,last2,result,comp);
                    #else
                        throw std::runtime_error( "The MultiCoreCpu version of merge is not enabled to be built! \n" );
                    #endif

                case bolt::amp::control::SerialCpu: 
				    #if defined(BOLT_DEBUG_LOG)
                    dblog->CodePathTaken(BOLTLOG::BOLT_MERGE,BOLTLOG::BOLT_SERIAL_CPU,"::Merge::SERIAL_CPU");
                    #endif
                    return std::merge(first1,last1,first2,last2,result,comp);

                default:
				   #if defined(BOLT_DEBUG_LOG)
                   dblog->CodePathTaken(BOLTLOG::BOLT_MERGE,BOLTLOG::BOLT_SERIAL_CPU,"::Merge::SERIAL_CPU");
                   #endif
                   return std::merge(first1,last1,first2,last2,result,comp);

                }

            }

            // This template is called after we detect random access iterators
            // This is called strictly for iterators that are derived from device_vector< T >::iterator
            template<typename DVInputIterator1,typename DVInputIterator2,typename DVOutputIterator, 
            typename StrictWeakCompare>
            DVOutputIterator merge_pick_iterator(bolt::amp::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                const DVOutputIterator& result,
                const StrictWeakCompare& comp,
                const std::string& cl_code,
                bolt::amp::device_vector_tag )
            {
                typedef typename std::iterator_traits<DVInputIterator1>::value_type iType1;
                typedef typename std::iterator_traits<DVInputIterator2>::value_type iType2;
                typedef typename std::iterator_traits<DVOutputIterator>::value_type oType;


                bolt::amp::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.
                if(runMode == bolt::amp::control::Automatic)
                {
                    runMode = ctl.getDefaultPathToRun();
                }
                #if defined(BOLT_DEBUG_LOG)
                BOLTLOG::CaptureLog *dblog = BOLTLOG::CaptureLog::getInstance();
                #endif
				
                switch(runMode)
                {
                case bolt::amp::control::Gpu :
				        #if defined(BOLT_DEBUG_LOG)
                        dblog->CodePathTaken(BOLTLOG::BOLT_MERGE,BOLTLOG::BOLT_GPU,"::Merge::GPU");
                        #endif
                        return detail::merge_enqueue( ctl, first1, last1,first2, last2, result, comp);
              
                case bolt::amp::control::MultiCoreCpu: 
                    #ifdef ENABLE_TBB
                    {
					  #if defined(BOLT_DEBUG_LOG)
                      dblog->CodePathTaken(BOLTLOG::BOLT_MERGE,BOLTLOG::BOLT_MULTICORE_CPU,"::Merge::MULTICORE_CPU");
                      #endif
                      typename bolt::amp::device_vector< iType1 >::pointer mergeInputBuffer1 =  first1.getContainer( ).data( );
                      typename bolt::amp::device_vector< iType2 >::pointer mergeInputBuffer2 =  first2.getContainer( ).data( );
                      typename bolt::amp::device_vector< oType >::pointer mergeResBuffer =  result.getContainer( ).data( );

                       bolt::btbb::merge(&mergeInputBuffer1[first1.m_Index],&mergeInputBuffer1[ last1.m_Index ],
                                               &mergeInputBuffer2[first2.m_Index],&mergeInputBuffer2[ last2.m_Index ],
                                               &mergeResBuffer[result.m_Index],comp);

                         return result + (last1 - first1) + (last2 - first2);
                    }
                    #else
                    {
                        throw std::runtime_error( "The MultiCoreCpu version of merge is not enabled to be built! \n" );
                    }
                    #endif

                case bolt::amp::control::SerialCpu: 
                    {
					  #if defined(BOLT_DEBUG_LOG)
                      dblog->CodePathTaken(BOLTLOG::BOLT_REDUCE,BOLTLOG::BOLT_SERIAL_CPU,"::Merge::SERIAL_CPU");
                      #endif
                      typename bolt::amp::device_vector< iType1 >::pointer mergeInputBuffer1 =  first1.getContainer( ).data( );
                      typename bolt::amp::device_vector< iType2 >::pointer mergeInputBuffer2 =  first2.getContainer( ).data( );
                      typename  bolt::amp::device_vector< oType >::pointer mergeResBuffer =  result.getContainer( ).data( );

                      std::merge(&mergeInputBuffer1[first1.m_Index],&mergeInputBuffer1[ last1.m_Index ],
                                               &mergeInputBuffer2[first2.m_Index],&mergeInputBuffer2[ last2.m_Index ],
                                              &mergeResBuffer[result.m_Index],comp);
                        return result + (last1 - first1) + (last2 - first2);
                    }

                default: /* Incase of runMode not set/corrupted */
                    {
					  #if defined(BOLT_DEBUG_LOG)
                      dblog->CodePathTaken(BOLTLOG::BOLT_REDUCE,BOLTLOG::BOLT_SERIAL_CPU,"::Merge::SERIAL_CPU");
                      #endif
					  
                      typename  bolt::amp::device_vector< iType1 >::pointer mergeInputBuffer1 =  first1.getContainer( ).data( );
                      typename  bolt::amp::device_vector< iType2 >::pointer mergeInputBuffer2 =  first2.getContainer( ).data( );
                      typename  bolt::amp::device_vector< oType >::pointer mergeResBuffer =  result.getContainer( ).data( );

                      std::merge(&mergeInputBuffer1[first1.m_Index],&mergeInputBuffer1[ last1.m_Index ],
                                      &mergeInputBuffer2[first2.m_Index],&mergeInputBuffer2[ last2.m_Index ],
                                      &mergeResBuffer[result.m_Index],comp);
                        return result + (last1 - first1) + (last2 - first2);


                    }

                }

            }

            // This template is called after we detect random access iterators
            // This is called strictly for iterators that are derived from device_vector< T >::iterator


#if 0
			template<typename DVInputIterator1,typename DVInputIterator2,typename DVOutputIterator, 
            typename StrictWeakCompare>
            DVOutputIterator merge_pick_iterator(bolt::amp::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                const DVOutputIterator& result,
                const StrictWeakCompare& comp,
                const std::string& cl_code,
                bolt::amp::fancy_iterator_tag )
            {
                typedef typename std::iterator_traits<DVInputIterator1>::value_type iType;
             

                bolt::amp::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.
                if(runMode == bolt::amp::control::Automatic)
                {
                    runMode = ctl.getDefaultPathToRun();
                }
				#if defined(BOLT_DEBUG_LOG)
                BOLTLOG::CaptureLog *dblog = BOLTLOG::CaptureLog::getInstance();
                #endif
                
                switch(runMode)
                {
                case bolt::amp::control::Gpu :
				        #if defined(BOLT_DEBUG_LOG)
                        dblog->CodePathTaken(BOLTLOG::BOLT_MERGE,BOLTLOG::BOLT_GPU,"::Merge::GPU");
                        #endif
                        return merge_enqueue( ctl, first1, last1,first2, last2, result, comp);
              
                case bolt::amp::control::MultiCoreCpu: 
                    #ifdef ENABLE_TBB
                    {
					  #if defined(BOLT_DEBUG_LOG)
                      dblog->CodePathTaken(BOLTLOG::BOLT_MERGE,BOLTLOG::BOLT_MULTICORE_CPU,"::Merge::MULTICORE_CPU");
                      #endif
                      return bolt::btbb::merge(first1, last1,first2, last2, result, comp);
                    }
                    #else
                    {
                       throw std::runtime_error( "The MultiCoreCpu version of merge is not enabled to be built! \n" );
                    }
                    #endif

                case bolt::amp::control::SerialCpu: 
				     #if defined(BOLT_DEBUG_LOG)
                     dblog->CodePathTaken(BOLTLOG::BOLT_REDUCE,BOLTLOG::BOLT_SERIAL_CPU,"::Merge::SERIAL_CPU");
                     #endif
                     return std::merge(first1, last1,first2, last2, result, comp);

                default: /* Incase of runMode not set/corrupted */
				    #if defined(BOLT_DEBUG_LOG)
                    dblog->CodePathTaken(BOLTLOG::BOLT_REDUCE,BOLTLOG::BOLT_SERIAL_CPU,"::Merge::SERIAL_CPU");
                    #endif
                    return std::merge(first1, last1,first2, last2, result, comp);

                }

            }        
            

#endif
            template<typename DVInputIterator1,typename DVInputIterator2,typename DVOutputIterator, 
            typename StrictWeakCompare>
            DVOutputIterator merge_detect_random_access(bolt::amp::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                const DVOutputIterator& result,
                const StrictWeakCompare& comp,
                std::random_access_iterator_tag)
            {
                return merge_pick_iterator( ctl, first1, last1, first2, last2,result, comp,
                    typename std::iterator_traits< DVInputIterator1 >::iterator_category( ) );
            }

            template<typename DVInputIterator1,typename DVInputIterator2,typename DVOutputIterator, 
            typename StrictWeakCompare>
            DVOutputIterator merge_detect_random_access(bolt::amp::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                const DVOutputIterator& result,
                const StrictWeakCompare& comp,
                std::input_iterator_tag)
            {
              //  TODO: It should be possible to support non-random_access_iterator_tag iterators,if we copied the data
              //  to a temporary buffer.  Should we?
                static_assert( std::is_same< DVInputIterator1, bolt::amp::input_iterator_tag  >::value,
                    "Bolt only supports random access iterator types" );
            }
            
      }
        template<typename InputIterator1 , typename InputIterator2 , typename OutputIterator > 
        OutputIterator merge (InputIterator1 first1, InputIterator1 last1, InputIterator2 first2, 
        InputIterator2 last2, OutputIterator result)
        {

           typedef typename std::iterator_traits<InputIterator1>::value_type iType1;
            return merge(bolt::amp::control::getDefault(), first1, last1, first2,last2,result, 
                bolt::amp::less<iType1>());
        };


        template<typename InputIterator1 , typename InputIterator2 , typename OutputIterator > 
        OutputIterator merge (bolt::amp::control &ctl,InputIterator1 first1, InputIterator1 last1,
            InputIterator2 first2, InputIterator2 last2, OutputIterator result)
        {
            typedef typename std::iterator_traits<InputIterator1>::value_type iType1;
            return merge(ctl, first1, last1, first2,last2,result, bolt::amp::less<iType1>() );
        };



        template<typename InputIterator1 , typename InputIterator2 , typename OutputIterator,
            typename StrictWeakCompare > 
        OutputIterator merge (InputIterator1 first1, InputIterator1 last1, InputIterator2 first2, 
        InputIterator2 last2, OutputIterator result,StrictWeakCompare comp)
        {
            return merge(bolt::amp::control::getDefault(), first1, last1, first2,last2,result, comp);
        };


        template<typename InputIterator1 , typename InputIterator2 , typename OutputIterator, 
         typename StrictWeakCompare >  
        OutputIterator merge (bolt::amp::control &ctl,InputIterator1 first1, InputIterator1 last1,
        InputIterator2 first2,InputIterator2 last2, OutputIterator result,StrictWeakCompare comp)
        {

            return detail::merge_detect_random_access(ctl, first1, last1, first2,last2,result, comp ,
                               typename std::iterator_traits< InputIterator1 >::iterator_category( ));

        };
 
    }

};






#endif //BOLT_AMP_MERGE_INL