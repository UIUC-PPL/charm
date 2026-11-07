module Main(mapping, showm, showpes, hist, main) where

import List(delete, sortBy)
import Monad(sequence)

matrix = 10000
blkSize = 500
numBlks = matrix `div` blkSize
numPes = 16

-- ``RealBlockCyclicMap''
-- mapping = let r = 2
--               m c1 c0 = c1 * numBlks + c0
--               val c1 c0 = (m c1 c0) `mod` (r * numPes) `div` r in
--         [val j i | i <- [0..numBlks-1], j <- [0..numBlks-1]]
-- end ``RealBlockCyclicMap''

mapping rows cols rotate stride x y =
        let tileYIndex = y `div` cols
            tileXIndex = x `div` rows
            xwithinPEtile = (x + tileYIndex * rotate) `mod` rows
            ywithinPEtile = y `mod` (cols `div` stride)
            subtileY = (y `mod` cols) `div` (cols `div` stride)
        in (xwithinPEtile * stride) + ywithinPEtile * rows * stride + subtileY

generateMapping mapping =
    [mapping i j | i <- [0..numBlks-1], j <- [0..numBlks-1]]

pval val = let str = show val in
           (++) str $ take ((-) 4 $ length str) (repeat ' ')

-- Visualize the processors mapped to each element of the chare
-- array. Shows the grid of the array.
showm m =
      let showm_ i j m | i < numBlks = do putStr . pval . head $ m
                                          showm_ (i+1) j $ tail m
                       | j < numBlks = do putStr "\n"
                                          showm_ 0 (j+1) m
                       | otherwise = return ()
      in showm_ 0 0 m

showblks pe = length . filter (==pe)

-- For each pe show the number of blocks assigned to it
showpes m =
        let showpes_ pe | pe < numPes = do putStrLn $ (show pe) ++ ": " ++ (show $ showblks pe m)
                                           showpes_ (pe + 1)
                        | otherwise = return ()
        in showpes_ 0

-- Show the number of processors who get some amout of blocks
hist m =
    let buildpes pe lst | pe < numPes = let blks = showblks pe m in
                                        buildpes (pe+1) $
                                        case lookup blks lst of
                                             Just amt -> (blks,amt+1):(delete (blks,amt) lst)
                                             Nothing -> (blks,1):lst
                        | otherwise = lst
    in sequence .
       map (\(blks,num) -> putStrLn $ (show num) ++ ":" ++ (show blks)) .
       sortBy (\a b -> compare (snd a) (snd b)) .
       buildpes 0 $ []

main = putStrLn "test"

{--main = do showm mapping
          putStrLn "\nshowpes:"
          showpes mapping
          putStrLn "hist:"
          hist mapping--}
